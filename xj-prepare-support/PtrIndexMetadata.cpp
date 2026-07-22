#include "PtrIndexMetadata.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <system_error>

namespace xj {

static llvm::json::Object pointerToJson(const PtrIndexPointerRecord &R) {
    llvm::json::Object O;
    O["name"] = R.name;
    O["index_var"] = R.index_var;
    O["param_index"] = R.param_index;
    O["moved"] = R.moved;
    O["base_text"] = R.base_text;
    O["min_offset"] = static_cast<int64_t>(R.min_offset);
    O["max_offset"] = static_cast<int64_t>(R.max_offset);
    O["variable_offsets"] = R.variable_offsets;
    O["handle_source"] = R.handle_source;
    O["returned_at_offset"] = R.returned_at_offset;
    O["dereferenced"] = R.dereferenced;
    llvm::json::Array Compared;
    for (const auto &C : R.compared_against)
        Compared.push_back(C);
    O["compared_against"] = std::move(Compared);
    return O;
}

static bool pointerFromJson(const llvm::json::Object &O,
                            PtrIndexPointerRecord &R) {
    auto Name = O.getString("name");
    auto IndexVar = O.getString("index_var");
    if (!Name || !IndexVar)
        return false;
    R.name = Name->str();
    R.index_var = IndexVar->str();
    R.param_index = static_cast<int>(O.getInteger("param_index").value_or(-1));
    R.moved = O.getBoolean("moved").value_or(false);
    R.base_text = O.getString("base_text").value_or("").str();
    R.min_offset = static_cast<long>(O.getInteger("min_offset").value_or(0));
    R.max_offset = static_cast<long>(O.getInteger("max_offset").value_or(0));
    R.variable_offsets = O.getBoolean("variable_offsets").value_or(false);
    R.handle_source = O.getString("handle_source").value_or("").str();
    R.returned_at_offset = O.getBoolean("returned_at_offset").value_or(false);
    R.dereferenced = O.getBoolean("dereferenced").value_or(false);
    if (const llvm::json::Array *Compared = O.getArray("compared_against")) {
        for (const auto &V : *Compared) {
            if (auto S = V.getAsString())
                R.compared_against.push_back(S->str());
        }
    }
    return true;
}

static llvm::json::Object sliceToJson(const PtrIndexSliceRecord &S) {
    llvm::json::Object O;
    O["slice_param_name"] = S.slice_param_name;
    O["slice_type"] = S.slice_type;
    O["pointee_type"] = S.pointee_type;
    O["base_param_index"] = S.base_param_index;
    O["end_param_index"] = S.end_param_index;
    O["len_param_index"] = S.len_param_index;
    O["lookback"] = static_cast<int64_t>(S.lookback);
    O["lookahead"] = static_cast<int64_t>(S.lookahead);
    O["inclusive_end"] = S.inclusive_end;
    O["return_type_changed"] = S.return_type_changed;
    llvm::json::Array Singletons;
    for (int I : S.singleton_param_indices)
        Singletons.push_back(I);
    O["singleton_param_indices"] = std::move(Singletons);
    return O;
}

static bool sliceFromJson(const llvm::json::Object &O, PtrIndexSliceRecord &S) {
    auto SliceType = O.getString("slice_type");
    auto PointeeType = O.getString("pointee_type");
    if (!SliceType || !PointeeType)
        return false;
    S.present = true;
    S.slice_param_name = O.getString("slice_param_name").value_or("arr").str();
    S.slice_type = SliceType->str();
    S.pointee_type = PointeeType->str();
    S.base_param_index =
        static_cast<int>(O.getInteger("base_param_index").value_or(-1));
    S.end_param_index =
        static_cast<int>(O.getInteger("end_param_index").value_or(-1));
    S.len_param_index =
        static_cast<int>(O.getInteger("len_param_index").value_or(-1));
    S.lookback = static_cast<long>(O.getInteger("lookback").value_or(0));
    S.lookahead = static_cast<long>(O.getInteger("lookahead").value_or(0));
    S.inclusive_end = O.getBoolean("inclusive_end").value_or(false);
    S.return_type_changed = O.getBoolean("return_type_changed").value_or(false);
    if (const llvm::json::Array *Singletons =
            O.getArray("singleton_param_indices")) {
        for (const auto &V : *Singletons) {
            if (auto I = V.getAsInteger())
                S.singleton_param_indices.push_back(static_cast<int>(*I));
        }
    }
    return true;
}

bool PtrIndexMetadata::writeToFile(const std::string &path) const {
    llvm::json::Object Root;
    Root["version"] = XJ_PTR_INDEX_METADATA_VERSION;

    llvm::json::Object Functions;
    for (const auto &[FnName, FnRec] : functions) {
        llvm::json::Array Pointers;
        for (const auto &P : FnRec.pointers)
            Pointers.push_back(pointerToJson(P));
        llvm::json::Object FnObj;
        FnObj["file"] = FnRec.file;
        FnObj["pointers"] = std::move(Pointers);
        if (FnRec.slice.present)
            FnObj["slice"] = sliceToJson(FnRec.slice);
        Functions[FnName] = std::move(FnObj);
    }
    Root["functions"] = std::move(Functions);

    llvm::json::Object GlobalReturns;
    for (const auto &[FnName, GR] : global_return_functions) {
        llvm::json::Object GRObj;
        GRObj["file"] = GR.file;
        GRObj["global_array_name"] = GR.global_array_name;
        GRObj["pointee_type"] = GR.pointee_type;
        GlobalReturns[FnName] = std::move(GRObj);
    }
    Root["global_return_functions"] = std::move(GlobalReturns);

    llvm::json::Array Globals;
    for (const auto &G : globals)
        Globals.push_back(pointerToJson(G));
    Root["globals"] = std::move(Globals);

    std::error_code EC;
    llvm::raw_fd_ostream OS(path, EC);
    if (EC)
        return false;
    OS << llvm::formatv("{0:2}", llvm::json::Value(std::move(Root)));
    return !OS.has_error();
}

bool PtrIndexMetadata::readFromFile(const std::string &path) {
    auto BufOrErr = llvm::MemoryBuffer::getFile(path);
    if (!BufOrErr)
        return false;
    auto Parsed = llvm::json::parse((*BufOrErr)->getBuffer());
    if (!Parsed) {
        llvm::consumeError(Parsed.takeError());
        return false;
    }
    const llvm::json::Object *Root = Parsed->getAsObject();
    if (!Root)
        return false;
    if (Root->getInteger("version").value_or(0) != XJ_PTR_INDEX_METADATA_VERSION)
        return false;

    functions.clear();
    global_return_functions.clear();
    globals.clear();

    if (const llvm::json::Object *Functions = Root->getObject("functions")) {
        for (const auto &[Key, Val] : *Functions) {
            const llvm::json::Object *FnObj = Val.getAsObject();
            if (!FnObj)
                return false;
            PtrIndexFunctionRecord FnRec;
            FnRec.file = FnObj->getString("file").value_or("").str();
            if (const llvm::json::Array *Pointers = FnObj->getArray("pointers")) {
                for (const auto &PV : *Pointers) {
                    const llvm::json::Object *PO = PV.getAsObject();
                    PtrIndexPointerRecord R;
                    if (!PO || !pointerFromJson(*PO, R))
                        return false;
                    FnRec.pointers.push_back(std::move(R));
                }
            }
            if (const llvm::json::Object *SliceObj = FnObj->getObject("slice")) {
                if (!sliceFromJson(*SliceObj, FnRec.slice))
                    return false;
            }
            functions[Key.str()] = std::move(FnRec);
        }
    }
    if (const llvm::json::Object *GlobalReturns =
            Root->getObject("global_return_functions")) {
        for (const auto &[Key, Val] : *GlobalReturns) {
            const llvm::json::Object *GRObj = Val.getAsObject();
            if (!GRObj)
                return false;
            PtrIndexGlobalReturnRecord GR;
            GR.file = GRObj->getString("file").value_or("").str();
            GR.global_array_name =
                GRObj->getString("global_array_name").value_or("").str();
            GR.pointee_type = GRObj->getString("pointee_type").value_or("").str();
            global_return_functions[Key.str()] = std::move(GR);
        }
    }
    if (const llvm::json::Array *Globals = Root->getArray("globals")) {
        for (const auto &GV : *Globals) {
            const llvm::json::Object *GO = GV.getAsObject();
            PtrIndexPointerRecord R;
            if (!GO || !pointerFromJson(*GO, R))
                return false;
            globals.push_back(std::move(R));
        }
    }
    return true;
}

} // namespace xj
