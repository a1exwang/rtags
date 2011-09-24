#include "ClangRunnable.h"
#include "Node.h"
#include "PreCompile.h"
#include <clang-c/Index.h>

static const bool disablePch = getenv("RTAGS_NO_PCH");
Node *ClangRunnable::sRoot = 0;
QMutex ClangRunnable::sPchMutex;
QMutex ClangRunnable::sTreeMutex;

struct PrecompileData {
    QList<Path> direct, all;
};

static inline void precompileHeaders(CXFile included_file, CXSourceLocation*,
                                     unsigned include_len, CXClientData client_data)
{
    if (!include_len)
        return;

    CXString filename = clang_getFileName(included_file);

    PrecompileData* data = reinterpret_cast<PrecompileData*>(client_data);
    Path rfn = Path::resolved(clang_getCString(filename));
    if (include_len == 1)
        data->direct.append(rfn);
    data->all.append(rfn);
    clang_disposeString(filename);
}


struct CursorNode {
    CursorNode(CXCursor c, CursorNode *p = 0)
        : cursor(c), parent(p), firstChild(0), nextSibling(0), lastChild(0)
    {
        if (p) {
            p->append(this);
        }
    }
    ~CursorNode()
    {
        CursorNode *c = firstChild;
        while (c) {
            CursorNode *tmp = c;
            c = c->nextSibling;
            delete tmp;
        }
    }
    int count() const
    {
        int ret = 1;
        for (CursorNode *c=firstChild; c; c = c->nextSibling) {
            ret += c->count();
        }
        return ret;
    }
    CXCursor cursor;
    CursorNode *parent, *firstChild, *nextSibling, *lastChild;
    void dump(int indent)
    {
        for (int i=0; i<indent; ++i) {
            printf(" ");
        }

        QString str;
        {
            QDebug dbg(&str);
            dbg << cursor;
            // CXCursor ref = clang_getCursorReferenced(cursor);
            // if (isValidCursor(ref))
            //     dbg << ref;
            // CXCursor can = clang_getCanonicalCursor(cursor);
            // if (isValidCursor(can))
            //     dbg << can;
            CXCursor p = clang_getCursorSemanticParent(cursor);
            if (isValidCursor(p))
                dbg << p;
        }

        str.remove("\"");
        printf("%s\n", str.toLocal8Bit().constData());
        for (CursorNode *c=firstChild; c; c = c->nextSibling) {
            c->dump(indent + 2);
        }
        fflush(stdout);
    }
    void append(CursorNode *c)
    {
        c->parent = this;
        if (lastChild) {
            lastChild->nextSibling = c;
            lastChild = c;
        } else {
            lastChild = firstChild = c;
        }
        nextSibling = 0;
    }
};
struct ComprehensiveTreeUserData {
    CursorNode *root;
    CursorNode *last;
    QVector<QPair<CXCursor, CursorNode*> > parents;
    CXCursor lastCursor;
};

/* There's a reason we don't use clang_equalCursors. It occasionally seems to
 * return 0 when the cursors seemingly are equal
 */

static bool operator==(const CXCursor &left, const CXCursor &right)
{
    return (left.kind == right.kind
            && clang_equalLocations(clang_getCursorLocation(left),
                                    clang_getCursorLocation(right)));
}

static CXChildVisitResult buildComprehensiveTree(CXCursor cursor, CXCursor parent, CXClientData data)
{
    CXSourceLocation location = clang_getCursorLocation(cursor);
    CXFile file = 0;
    clang_getInstantiationLocation(location, &file, 0, 0, 0);
    // ### is this safe?
    if (!file)
        return CXChildVisit_Continue;

    // qDebug() << cursor << parent;
    ComprehensiveTreeUserData *u = reinterpret_cast<ComprehensiveTreeUserData*>(data);
    CursorNode *p = 0;
    if (!u->root) {
        u->root = new CursorNode(parent);
        p = u->root;
        u->parents.append(qMakePair(parent, u->root));
        u->lastCursor = cursor;
    } else {
        Q_ASSERT(u->last);
        if (parent == u->lastCursor) {
            p = u->last;
            Q_ASSERT(p);
            u->parents.append(qMakePair(parent, p));
        } else {
            for (int i=u->parents.size() - 1; i>=0; --i) {
                if (parent == u->parents.at(i).first) {
                    p = u->parents.at(i).second;
                    u->parents.resize(i + 1);
                    break;
                }
            }
        }
    }
    if (!p) {
        qDebug() << parent.kind << u->lastCursor.kind
                 << parent.data[0] << u->lastCursor.data[0]
                 << parent.data[1] << u->lastCursor.data[1]
                 << parent.data[2] << u->lastCursor.data[2];

        qWarning() << "crashing cursor is" << cursor
                   << "\nparent is" << parent
                   << "\nlastCursor is" << u->lastCursor
            // << "\nparents are" << u->parents
                   << "\nparent and lastCursor are equal" << clang_equalCursors(parent, u->lastCursor)
                   << "\ncursorId(parent)" << Location(parent).toString()
                   << "\ncursorId(u->lastCursor)" << Location(u->lastCursor).toString();
    }
    Q_ASSERT(p);
    u->last = new CursorNode(cursor, p);
    u->lastCursor = cursor;
    switch (clang_getCursorKind(cursor)) {
    case CXCursor_EnumConstantDecl:
    case CXCursor_MemberRefExpr:
    case CXCursor_DeclRefExpr:
        return CXChildVisit_Continue; //
    default:
        break;
    }
    return CXChildVisit_Recurse;
}

ClangRunnable::ClangRunnable(const Path &file, const GccArguments &args)
    : mFile(file), mArgs(args)
{
    setAutoDelete(true);
}

void ClangRunnable::init()
{
    sRoot = new Node;
}

void ClangRunnable::cleanup()
{
    delete sRoot;
    sRoot = 0;
}

void ClangRunnable::run()
{
    CXIndex index = clang_createIndex(1, 0);

    QElapsedTimer timer;
    timer.start();
    QVector<const char*> args;
    const QList<QByteArray> compilerOptions = mArgs.includePaths() + mArgs.arguments("-D");
    const int compilerOptionsCount = compilerOptions.count();

    CXTranslationUnit unit = 0;
    enum { WithPCH, WithoutPCH };
    for (int i=0; i<2 && !unit; ++i) {
        PreCompile *precompile = 0;
        if (!disablePch && i == WithPCH) {
            sPchMutex.lock();
            precompile = PreCompile::get(compilerOptions);
        }
        Path pchfile;
        int argCount = compilerOptions.size();
        if (i == WithPCH) {
            if (!precompile)
                continue;
            if (mArgs.language() != GccArguments::LangCPlusPlus) {
                sPchMutex.unlock();
                continue;
            }
            pchfile = precompile->filename().toLocal8Bit();
            if (!pchfile.isFile()) {
                sPchMutex.unlock();
                continue;
            }
            argCount += 2;
        }
        if (!disablePch && i == WithPCH)
            sPchMutex.unlock();

        // ### this allocates more than it needs to strictly speaking. In fact a
        // ### lot of files will have identical options so we could even reuse
        // ### the actual QVarLengthArray a lot of times.
        if (args.size() < argCount)
            args.resize(argCount);
        for (int a=0; a<compilerOptionsCount; ++a) {
            args[a] = compilerOptions.at(a).constData();
        }
        if (i == WithPCH) {
            args[compilerOptionsCount] = "-pch";
            args[compilerOptionsCount + 1] = pchfile.constData();
        }

        do {
            const time_t before = mFile.lastModified();
            // qDebug() << "parsing file" << mFile << (i == WithPCH ? "with PCH" : "without PCH");
            Q_ASSERT(!args.contains(0));
            // for (int i=0; i<argCount; ++i) {
            //     printf("%d [%s]\n", i, args.constData()[i]);
            // }

            // qDebug() << "calling parse" << mFile << args;
            unit = clang_parseTranslationUnit(index, mFile.constData(),
                                              args.constData(), argCount, 0, 0,
                                              // CXTranslationUnit_NestedMacroExpansions
                                              CXTranslationUnit_DetailedPreprocessingRecord);
            if (unit && before != mFile.lastModified())
                continue;
        } while (false);
        if (!unit) {
            qWarning("Couldn't parse %s", mFile.constData());
            QByteArray clangLine = "clang";
            if (mArgs.language() == GccArguments::LangCPlusPlus)
                clangLine += "++";
            for (int j=0; j<argCount; ++j) {
                clangLine += ' ';
                clangLine += args.at(j);
            }
            clangLine += ' ' + mFile;
            qWarning("[%s]", clangLine.constData());
        } else {
            PrecompileData pre;
            clang_getInclusions(unit, precompileHeaders, &pre);
            // qDebug() << mFile << pre.direct << pre.all;
            if (precompile) {
                precompile->add(pre.direct, pre.all);
            }
            // mFiles[mFile] = mFile.lastModified();
            // emit fileParsed(mFile, unit);
            // const QSet<Path> deps = pre.all.toSet();
            // foreach(const Path &dep, deps) {
            //     mFiles[dep] = dep.lastModified();
            // }
            // if (mFileManager->addDependencies(mFile, deps))
            //     emit dependenciesAdded(deps);
            qDebug() << "file was parsed" << mFile << timer.elapsed() << "ms"
                     << (i == WithPCH ? "with PCH" : "without PCH") << compilerOptions;
        }
    }
    if (unit) {
        CXCursor rootCursor = clang_getTranslationUnitCursor(unit);
        ComprehensiveTreeUserData ud;
        ud.last = ud.root = 0;
        ud.lastCursor = clang_getNullCursor();
        clang_visitChildren(rootCursor, buildComprehensiveTree, &ud);
#ifndef QT_NO_DEBUG
        const QByteArray dump = qgetenv("RTAGS_DUMP");
        if (ud.root && (dump == "1" || dump.contains(mFile.fileName()))) {
            ud.root->dump(0);
            printf("Tree done\n");
            fflush(stdout);
            sleep(1);
        }
#endif
        QHash<QByteArray, PendingReference> references;
        if (ud.root) {
            int old;
            {
                QMutexLocker lock(&sTreeMutex);
                old = Node::sNodes.size();
                buildTree(sRoot, ud.root, references);
                for (QHash<QByteArray, PendingReference>::const_iterator it = references.begin(); it != references.end(); ++it) {
                    const PendingReference &p = it.value();
                    addReference(p.node, it.key(), p.location);
                }
            }
            delete ud.root;
            qDebug() << "added" << (Node::sNodes.size() - old) << "nodes for" << mFile << ". Total" << Node::sNodes.size();
        }
        clang_disposeTranslationUnit(unit);
    }
    clang_disposeIndex(index);
    emit finished();
}

void ClangRunnable::buildTree(Node *parent, CursorNode *c, QHash<QByteArray, PendingReference> &references)
{
    Q_ASSERT(c);
    if (clang_getCursorKind(c->cursor) == CXCursor_MacroExpansion) {
        const Location loc(c->cursor);
        Q_ASSERT(!loc.isNull());
        const QByteArray id = loc.toString();
        if (Node::sNodes.contains(id))
            return;
        const QByteArray symbolName = eatString(clang_getCursorSpelling(c->cursor));
        for (CursorNode *cn = c->parent->firstChild; cn; cn = cn->nextSibling) {
            if (clang_getCursorKind(cn->cursor) == CXCursor_MacroDefinition
                && symbolName == eatString(clang_getCursorSpelling(cn->cursor))) {
                const QByteArray macroDefinitionId = Location(cn->cursor).toString();
                Node *parent = Node::sNodes.value(macroDefinitionId);
                Q_ASSERT(parent);
                new Node(parent, Reference, c->cursor, loc, id);
                return;
            }
        }
    }

    const NodeType type = Node::nodeTypeFromCursor(c->cursor);
    if (type == Reference) {
        const Location loc(c->cursor);
        if (loc.exists()) {
            const QByteArray id = loc.toString();
            if (!Node::sNodes.contains(id)) {
                const PendingReference r = { c, loc };
                references[id] = r;
            }
        }
    } else {
        if (c->parent && type != Invalid) {
            const Location loc(c->cursor);
            if (loc.exists()) {
                const QByteArray id = loc.toString();
                if (Node::sNodes.contains(id))
                    return;
                // ### may not need to do this for all types of nodes
                CXCursor realParent = clang_getCursorSemanticParent(c->cursor);
                if (isValidCursor(realParent) && !clang_equalCursors(realParent, c->parent->cursor)) {
                    const QByteArray parentId = Location(realParent).toString();
                    parent = Node::sNodes.value(parentId, parent);
                }

                parent = new Node(parent, type, c->cursor, loc, id);
            }
        }
        for (CursorNode *child=c->firstChild; child; child = child->nextSibling) {
            buildTree(parent, child, references);
        }
    }
}

void ClangRunnable::addReference(CursorNode *c, const QByteArray &id, const Location &loc)
{
    if (Node::sNodes.contains(id)) {
        qWarning() << "Turns out" << c->cursor << "already exists"
                   << Node::sNodes.value(id)->symbolName << nodeTypeToName(Node::sNodes.value(id)->type, Normal)
                   << Node::sNodes.value(id)->location;
        return;
    }
    if (Node::nodeTypeFromCursor(c->cursor) != Invalid && loc.exists()) {
        const CXCursorKind kind = clang_getCursorKind(c->cursor);

        CXCursor ref = clang_getCursorReferenced(c->cursor);
        if (clang_equalCursors(ref, c->cursor) && (kind == CXCursor_ClassDecl || kind == CXCursor_StructDecl)) { // ### namespace too?
            ref = clang_getCursorDefinition(ref);
        }

        if (!isValidCursor(ref)) {
            if (kind != CXCursor_MacroExpansion && kind != CXCursor_ClassDecl && kind != CXCursor_StructDecl)
                qWarning() << "Can't get valid cursor for" << c->cursor << "child of" << c->parent->cursor;
            return;
        }

        const CXCursorKind refKind = clang_getCursorKind(ref);
        if (kind == CXCursor_DeclRefExpr) {
            switch (refKind) {
            case CXCursor_ParmDecl:
            case CXCursor_VarDecl:
            case CXCursor_FieldDecl:
            case CXCursor_CXXMethod:
            case CXCursor_EnumConstantDecl:
            case CXCursor_FunctionDecl:
                break;
            case CXCursor_NonTypeTemplateParameter:
                return;
            default:
                qDebug() << "throwing out this pending CXCursor_DeclRefExpr" << c->cursor << ref;
                return;
            }
        }
        const QByteArray refId = Location(ref).toString();
        Node *refNode = Node::sNodes.value(refId);
        if (!refNode) {
            // qWarning() << "Can't find referenced node" << c->cursor << ref << refId;
            return;
        }
        if (refNode->type == MethodDefinition) {
            Node *decl = refNode->methodDeclaration();
            if (decl)
                refNode = decl;
        }
        Q_ASSERT(!Node::sNodes.contains(id));
        new Node(refNode, Reference, c->cursor, loc, id);
        Q_ASSERT(Node::sNodes.contains(id));
    }

    for (CursorNode *child=c->firstChild; child; child = child->nextSibling) {
        const Location l(child->cursor);
        addReference(child, l.toString(), l);
        // if (typeFromCursor(child->cursor) != Invalid) {
        //     qWarning() << "This node is a child of a ref" << child->cursor
        //                << c->cursor << ref;
        // }
    }

}
