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
    O["base_text"] = R.base_text;
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
    R.base_text = O.getString("base_text").value_or("").str();
    return true;
}

bool PtrIndexMetadata::writeToFile(const std::string &path) const {
    llvm::json::Object Root;

    llvm::json::Object Functions;
    for (const auto &[FnName, FnRec] : functions) {
        llvm::json::Array Pointers;
        for (const auto &P : FnRec.pointers)
            Pointers.push_back(pointerToJson(P));
        llvm::json::Object FnObj;
        FnObj["file"] = FnRec.file;
        FnObj["pointers"] = std::move(Pointers);
        Functions[FnName] = std::move(FnObj);
    }
    Root["functions"] = std::move(Functions);

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

    functions.clear();

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
            functions[Key.str()] = std::move(FnRec);
        }
    }
    return true;
}

} // namespace xj
