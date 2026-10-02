// EditForest.cpp — see EditForest.h for what this guarantees and why.

#include "EditForest.h"

#include "clang/Lex/Lexer.h"

#include <algorithm>
#include <cassert>

using namespace clang;

namespace xj
{

    std::optional<FileRange> fileRangeOf(SourceRange Range,
                                         const SourceManager &SM,
                                         const LangOptions &LO)
    {
        // makeFileCharRange yields the past-the-end position directly, and
        // reports invalid for a range whose ends it cannot map to one
        // file.
        CharSourceRange R = Lexer::makeFileCharRange(
            CharSourceRange::getTokenRange(Range), SM, LO);
        if (R.isInvalid())
            return std::nullopt;

        auto [File, Begin] = SM.getDecomposedLoc(R.getBegin());
        auto [EndFile, End] = SM.getDecomposedLoc(R.getEnd());
        if (File != EndFile || End <= Begin)
            return std::nullopt;
        return FileRange{File, Begin, End};
    }

    // True when `A` starts before `B`: in an earlier file, or earlier in
    // the same one.
    static bool startsBefore(const FileRange &A, const FileRange &B)
    {
        return A.File != B.File ? A.File < B.File : A.Begin < B.Begin;
    }

    EditForest::EditForest(const SourceManager &SM, const LangOptions &LO,
                           RenderFn Render)
        : SM(SM), LO(LO), Render(std::move(Render)) {}

    EditForest::EditId EditForest::add(FileRange Range)
    {
        // An edit rendered before this one was added inside it rendered
        // without it, so that rendering no longer stands.
        if (AnyRendered)
            for (Edit &E : Edits)
                if (E.Rendered && E.Range.File == Range.File &&
                    E.Range.Begin <= Range.Begin && Range.End <= E.Range.End)
                {
                    E.Rendered = false;
                    E.Stale = true;
                }
        Edits.push_back({Range});
        Sorted = false;
        return Edits.size() - 1;
    }

    // Sort the edits so that an edit comes before everything nested inside
    // it.
    void EditForest::sortEdits()
    {
        Order.resize(Edits.size());
        for (EditId Id = 0; Id < Edits.size(); Id++)
            Order[Id] = Id;
        std::stable_sort(Order.begin(), Order.end(), [&](EditId A, EditId B)
                         {
                             const FileRange &X = Edits[A].Range;
                             const FileRange &Y = Edits[B].Range;
                             if (startsBefore(X, Y))
                                 return true;
                             if (startsBefore(Y, X))
                                 return false;
                             return X.End > Y.End; });
        Sorted = true;
    }

    std::string EditForest::renderEdit(EditId Id)
    {
        Edits[Id].Rendered = true;
        Edits[Id].Stale = false;
        Edits[Id].Rendering = true;
        AnyRendered = true;
        Depth++;
        std::string Text = Render(Id, NestedEditRenderer(*this, Id));
        Depth--;
        Edits[Id].Rendering = false;
        return Text;
    }

    // The text of `Range`: the original source, with each edit that lies
    // wholly inside `Range` replaced by its rendering.
    //
    // Does not render the edit corresponding to Range, to avoid
    // recursion (this method is used in the callback to produce
    // the text for a given edit)
    //
    // An edit not wholly inside `Range` (it contains `Range`, or starts or
    // ends outside it) is not rendered.
    //
    // Assumes `Order` is sorted
    std::string EditForest::splice(const FileRange &Range)
    {
        llvm::StringRef Buffer = SM.getBufferData(Range.File);

        std::string Out;
        unsigned Cursor = Range.Begin;
        // Start at the first edit beginning in `Range`; an edit beginning
        // before `Range` cannot lie wholly inside it.
        auto It = std::lower_bound(Order.begin(), Order.end(), Range,
                                   [&](EditId Id, const FileRange &R)
                                   { return startsBefore(Edits[Id].Range, R); });
        for (; It != Order.end(); ++It)
        {
            const FileRange &R = Edits[*It].Range;
            if (R.File != Range.File || R.Begin >= Range.End)
                break;
            // The edits form a forest.
            // Suppose we have { A, B, C } within the range of `Range`.
            // When we call `renderEdit`, we will recursively render B and C.
            // `Cursor` records the extent of the whole range so that we know
            // to skip B and C once the top level call to renderEdit(A) returns.
            if (R.End > Range.End || Edits[*It].Rendering || R.Begin < Cursor)
                continue;
            Out += Buffer.slice(Cursor, R.Begin);
            Out += renderEdit(*It);
            Cursor = R.End;
        }
        Out += Buffer.slice(Cursor, Range.End);
        return Out;
    }

    std::string EditForest::textOf(SourceRange Range)
    {
        if (!Sorted)
            sortEdits();
        if (auto R = fileRangeOf(Range, SM, LO))
            return splice(*R);
        return Lexer::getSourceText(CharSourceRange::getTokenRange(Range), SM, LO)
            .str();
    }

    std::string EditForest::NestedEditRenderer::text(SourceRange Range) const
    {
        assert(Forest.Edits[Id].Rendering &&
               "NestedEditRenderer used outside its callback");
        return Forest.textOf(Range);
    }

    std::string EditForest::text(SourceRange Range)
    {
        assert(Depth == 0 &&
               "text() inside a callback: use the NestedEditRenderer");
        return textOf(Range);
    }

    std::vector<EditForest::Replacement> EditForest::renderRoots()
    {
        if (!Sorted)
            sortEdits();

        // `Order` puts an edit ahead of what it contains, so an edit
        // starting before the last root ends is nested in that root.
        std::vector<Replacement> Roots;
        FileID File;
        unsigned Reach = 0;
        for (EditId Id : Order)
        {
            const FileRange &R = Edits[Id].Range;
            if (R.File != File)
            {
                File = R.File;
                Reach = 0;
            }
            if (R.Begin < Reach)
                continue;
            Reach = R.End;
            Roots.push_back({R, renderEdit(Id)});
        }
        return Roots;
    }

    std::vector<EditForest::Problem> EditForest::verify()
    {
        if (!Sorted)
            sortEdits();
        std::vector<Problem> Problems;

        // Sweep with a stack of the edits still open at each start, for
        // the edits that do not nest.
        std::vector<EditId> Open;
        for (EditId Id : Order)
        {
            const FileRange &R = Edits[Id].Range;
            while (!Open.empty() && (Edits[Open.back()].Range.File != R.File ||
                                     Edits[Open.back()].Range.End <= R.Begin))
                Open.pop_back();

            if (!Open.empty())
            {
                const FileRange &Outer = Edits[Open.back()].Range;
                if (R.End > Outer.End)
                {
                    Problems.push_back(
                        {Id, "overlaps another without nesting inside it"});
                    continue;
                }
                if (R.Begin == Outer.Begin && R.End == Outer.End)
                {
                    Problems.push_back({Id, "covers the same text as another"});
                    continue;
                }
            }
            Open.push_back(Id);
        }

        for (EditId Id = 0; Id < Edits.size(); Id++)
            if (!Edits[Id].Rendered)
                Problems.push_back(
                    {Id, Edits[Id].Stale
                             ? "was rendered before an edit was added inside "
                               "it, and not since"
                             : "was neither applied nor rendered into the "
                               "edit containing it"});
        return Problems;
    }

} // namespace xj
