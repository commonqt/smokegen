#include "astconsumer.h"
#include "ppcallbacks.h"

void SmokegenASTConsumer::Initialize(clang::ASTContext &ctx) {
    auto ppCallbacks = std::make_unique<SmokegenPPCallbacks>(ci.getPreprocessor());
    ci.getPreprocessor().addPPCallbacks(std::move(ppCallbacks));
}

bool SmokegenASTConsumer::HandleTopLevelDecl(clang::DeclGroupRef DR) {

    for (clang::DeclGroupRef::iterator b = DR.begin(), e = DR.end(); b != e; ++b) {
        // Traverse the declaration using our AST visitor.
        Visitor.TraverseDecl(*b);
    }
    return true;
}
