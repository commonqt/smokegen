#include <clang/Sema/SemaDiagnostic.h>

#include "frontendaction.h"
#include "astconsumer.h"
#include <QtDebug>

std::unique_ptr<clang::ASTConsumer>
SmokegenFrontendAction::CreateASTConsumer(clang::CompilerInstance &CI, clang::StringRef file) {
    qDebug() << "CreateASTConsumer for" << QString::fromStdString(file.str());
    try {
        // Parsing function bodies can cause global template functions to be
        // instantiated unnecessarily
        CI.getFrontendOpts().SkipFunctionBodies = true;
        CI.getDiagnostics().setSeverity(clang::diag::warn_undefined_inline, clang::diag::Severity::Ignored, clang::SourceLocation());

        auto consumer = std::make_unique<SmokegenASTConsumer>(CI);
        qDebug() << "CreateASTConsumer succeeded for" << QString::fromStdString(file.str());
        return consumer;
    } catch (const std::exception &e) {
        qCritical() << "CreateASTConsumer threw exception:" << e.what();
        throw;
    } catch (...) {
        qCritical() << "CreateASTConsumer threw unknown exception";
        throw;
    }
}
