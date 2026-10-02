// EditForest — source edits that may nest.
//
// Manages rendering a collection of edits that may nest such that
// sub edits are not dropped.
//
// Usage: The forest knows an edit only by its id, which is the index it
// was added at, so a tool keeps what each edit means in a table the id
// indexes, pushed in step with add():
//
//     std::vector<const MyEdit *> Planned;   // Planned[Id] is edit Id
//     using Renderer = xj::EditForest::NestedEditRenderer;
//     xj::EditForest Forest(SM, LO, [&](xj::EditForest::EditId Id,
//                                       const Renderer &In) {
//         const MyEdit &E = *Planned[Id];
//         // The replacement for E. A sub-expression E keeps is taken
//         // through In.text(), never from the source buffer, so the
//         // edits nested inside it come back rendered.
//         return "f(" + In.text(E.Arg->getSourceRange()) + ")";
//     });
//
//     for (const MyEdit &E : MyEdits) {
//         auto R = xj::fileRangeOf(E.Node->getSourceRange(), SM, LO);
//         if (!R)
//             continue;                      // unrewritable: see fileRangeOf
//         xj::EditForest::EditId Id = Forest.add(*R);
//         assert(Id == Planned.size());
//         Planned.push_back(&E);
//     }
//
//     std::vector<xj::EditForest::Replacement> Roots = Forest.renderRoots();
//     std::vector<xj::EditForest::Problem> Problems = Forest.verify();
//     if (!Problems.empty()) {
//         for (const xj::EditForest::Problem &P : Problems)
//             /* report P.What at Forest.rangeOf(P.Edit).begin(SM) */;
//         return;                       // a tool bug: leave the file alone
//     }
//     for (const xj::EditForest::Replacement &Root : Roots)
//         Rewrite.ReplaceText(Root.Range.begin(SM), Root.Range.size(),
//                             Root.Text);    // clang::Rewriter Rewrite

#ifndef XJ_PREPARE_SUPPORT_EDIT_FOREST_H
#define XJ_PREPARE_SUPPORT_EDIT_FOREST_H

#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace xj
{

    // A span of one file's text, in offsets into the original buffer.
    struct FileRange
    {
        clang::FileID File;
        unsigned Begin = 0;
        unsigned End = 0; // past-the-end

        unsigned size() const { return End - Begin; }

        clang::SourceLocation begin(const clang::SourceManager &SM) const
        {
            return SM.getLocForStartOfFile(File).getLocWithOffset(Begin);
        }
    };

    // The file text `Range` covers, taken as a token range, or nullopt
    // when clang::Rewriter could not edit it: it is empty, or its ends come
    // from different macro expansions or different files.
    //
    // A range that begins or ends inside a macro expansion is mapped onto
    // the macro's use where that is possible, so `p = NULL` is the text
    // from `p` to the end of the name `NULL`.
    std::optional<FileRange> fileRangeOf(clang::SourceRange Range,
                                         const clang::SourceManager &SM,
                                         const clang::LangOptions &LO);

    class EditForest
    {
    public:
        // Names an edit: the order it was added in, from 0.
        using EditId = size_t;

        // Handed to the callback to render an edit's subexpressions
        class NestedEditRenderer
        {
        public:
            // The source text of `Range`, with each edit nested in the
            // edit being rendered, and lying inside `Range`, replaced by
            // its rendering. `Range` is normally a sub-expression the edit
            // keeps, or the edit's own range.
            std::string text(clang::SourceRange Range) const;

        private:
            friend class EditForest;
            NestedEditRenderer(EditForest &Forest, EditId Id)
                : Forest(Forest), Id(Id) {}
            EditForest &Forest;
            EditId Id;
        };

        // The replacement text for one edit. The id is the one add()
        // returned for the edit; sub-expressions the replacement keeps
        // are taken through `In.text()`.
        using RenderFn = std::function<std::string(
            EditId Id, const NestedEditRenderer &In)>;

        // A root edit. Roots are disjoint, so they can
        // be handed to clang::Rewriter in any order.
        struct Replacement
        {
            FileRange Range;
            std::string Text;
        };

        // An edit verify() found the plan would lose, and why.
        struct Problem
        {
            EditId Edit;
            const char *What;
        };

        EditForest(const clang::SourceManager &SM, const clang::LangOptions &LO,
                   RenderFn Render);

        // Plan an edit replacing `Range`. Allowed after rendering: an
        // edit already rendered whose range contains `Range` was rendered
        // without the new edit, so it counts as unrendered again until
        // it is rendered once more.
        EditId add(FileRange Range);

        // The source text of `Range`, with each edit inside it replaced by
        // its rendering.
        std::string text(clang::SourceRange Range);

        // Render every root, in source order, as the edits stand now.
        std::vector<Replacement> renderRoots();

        // The edits the plan would lose if applied now; the plan is sound
        // when there are none. Call it after all rendering, since
        // rendering is what shows an edit was reached. An edit is lost
        // when:
        //   - it and another overlap without one nesting inside the other,
        //     or cover the same text, so that neither is outer or inner;
        //   - it was never rendered: no root, and no text() call from its
        //     container's callback or from the tool, covered it;
        //   - it was rendered, but another was then added inside it and
        //     it has not been rendered since.
        // Idempotent: a repeat call recomputes the lost edits.
        std::vector<Problem> verify();

        const FileRange &rangeOf(EditId Id) const { return Edits[Id].Range; }

    private:
        struct Edit
        {
            FileRange Range;
            bool Rendered = false;
            // Rendered once, then an edit was added inside it; cleared by
            // rendering it again.
            bool Stale = false;
            bool Rendering = false;
        };

        void sortEdits();
        std::string renderEdit(EditId Id);
        std::string textOf(clang::SourceRange Range);
        std::string splice(const FileRange &Range);

        const clang::SourceManager &SM;
        const clang::LangOptions &LO;
        RenderFn Render;
        std::vector<Edit> Edits;
        // Edit ids sorted by (file, start, width), so that an edit comes
        // before everything nested inside it.
        std::vector<EditId> Order;
        bool Sorted = false;
        // How many callbacks are running.
        unsigned Depth = 0;
        // Whether anything has been rendered yet; until then add() has
        // nothing to invalidate.
        bool AnyRendered = false;
    };

} // namespace xj

#endif // XJ_PREPARE_SUPPORT_EDIT_FOREST_H
