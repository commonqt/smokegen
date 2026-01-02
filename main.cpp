/*
    Copyright (C) 2009 Arno Rehn <arno@arnorehn.de>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <QCoreApplication>
#include <QList>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>

#include <QDomDocument>
#include <QDomElement>
#include <QDomNode>

#include <QtDebug>

#include <iostream>
#include <memory>
#if defined(_WIN32)
# if !defined(WIN32_LEAN_AND_MEAN)
#   define WIN32_LEAN_AND_MEAN
# endif
# if !defined(NOMINMAX)
#   define NOMINMAX
# endif
# include <windows.h>
#endif

#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/ArgumentsAdjusters.h>
#include <clang/Tooling/Tooling.h>

#include <clang/Basic/FileManager.h>
#include <clang/Basic/FileSystemOptions.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <clang/Basic/DiagnosticOptions.h>

#include "options.h"
#include "type.h"
#include "config.h"
#include "frontendaction.h"
#include "embedded_includes.h"


using GenerateFn = int (*)();

static void showUsage()
{
    std::cout << 
    "Usage: smokegen [options] [-clangOptions [options]] -- <header files>" << std::endl <<
    "Possible command line options are:" << std::endl <<
    "    -I <include dir>" << std::endl <<
    "    -d <path to file containing #defines>" << std::endl <<
    "    -dm <list of macros that should be ignored>" << std::endl <<
    "    -g <generator to use>" << std::endl <<
    "    -qt enables Qt-mode (special treatment of QFlags)" << std::endl <<
    "    -t resolve typedefs" << std::endl <<
    "    -o <output dir>" << std::endl <<
    "    -config <config file>" << std::endl <<
    "    -clangOptions <flags to pass to the clang tool>" << std::endl <<
    "    -h shows this message" << std::endl;
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        showUsage();
        return EXIT_SUCCESS;
    }

    QCoreApplication app(argc, argv);
    const QStringList& args = app.arguments();

    QFileInfo configFile;
    QString generator;
    bool addHeaders = false;
    bool addClangOptions = false;
    bool hasCommandLineGenerator = false;
    QStringList classes;

    ParserOptions::notToBeResolved << "FILE";

    // Store clang options separately to avoid lifetime issues
    std::vector<std::string> clangOptions;

    for (int i = 1; i < args.count(); i++) {
        if ((args[i] == "-I" || args[i] == "-d" || args[i] == "-dm" ||
             args[i] == "-g" || args[i] == "-config") && i + 1 >= args.count())
        {
            qCritical() << "not enough parameters for option" << args[i];
            return EXIT_FAILURE;
        }
        if (args[i] == "-I") {
            QString d = args[++i];
            if (!d.isEmpty())
                ParserOptions::includeDirs << QDir(d);
        } else if (args[i] == "-config") {
            configFile = QFileInfo(args[++i]);
        } else if (args[i] == "-d") {
            ParserOptions::definesList = QFileInfo(args[++i]);
        } else if (args[i] == "-dm") {
            ParserOptions::dropMacros += args[++i].split(',');
        } else if (args[i] == "-g") {
            generator = args[++i];
            hasCommandLineGenerator = true;
        } else if ((args[i] == "-h" || args[i] == "--help") && argc == 2) {
            showUsage();
            return EXIT_SUCCESS;
        } else if (args[i] == "-t") {
            ParserOptions::resolveTypedefs = true;
        } else if (args[i] == "-qt") {
            ParserOptions::qtMode = true;
        } else if (args[i] == "-clangOptions") {
            addClangOptions = true;
        } else if (args[i] == "--") {
            addClangOptions = false;
            addHeaders = true;
        } else if (addClangOptions) {
            clangOptions.push_back(args[i].toStdString());
        } else if (addHeaders) {
            ParserOptions::headerList << QFileInfo(args[i]);
        }
    }
        
    if (configFile.exists()) {
        QFile file(configFile.filePath());
        file.open(QIODevice::ReadOnly);
        QDomDocument doc;
        doc.setContent(file.readAll());
        file.close();
        QDomElement root = doc.documentElement();
        QDomNode node = root.firstChild();
        while (!node.isNull()) {
            QDomElement elem = node.toElement();
            if (elem.isNull()) {
                node = node.nextSibling();
                continue;
            }
            if (elem.tagName() == "resolveTypedefs") {
                ParserOptions::resolveTypedefs = (elem.text() == "true");
            } else if (elem.tagName() == "qtMode") {
                ParserOptions::qtMode = (elem.text() == "true");
            } else if (!hasCommandLineGenerator && elem.tagName() == "generator") {
                generator = elem.text();
            } else if (elem.tagName() == "includeDirs") {
                QDomNode dir = elem.firstChild();
                while (!dir.isNull()) {
                    QDomElement elem = dir.toElement();
                    if (elem.isNull()) {
                        dir = dir.nextSibling();
                        continue;
                    }
                    if (elem.tagName() == "dir") {
                        QString p = elem.text();
                        if (!p.isEmpty())
                            ParserOptions::includeDirs << QDir(p);
                    }
                    else if (elem.tagName() == "framework") {
                        QString p = elem.text();
                        if (!p.isEmpty())
                            ParserOptions::frameworkDirs << QDir(p);
                    }
                    dir = dir.nextSibling();
                }
            } else if (elem.tagName() == "definesList") {
                // reference to an external file, so it can be auto-generated
                ParserOptions::definesList = QFileInfo(elem.text());
            } else if (elem.tagName() == "dropMacros") {
                QDomNode macro = elem.firstChild();
                while (!macro.isNull()) {
                    QDomElement elem = macro.toElement();
                    if (elem.isNull()) {
                        macro = macro.nextSibling();
                        continue;
                    }
                    if (elem.tagName() == "name") {
                        ParserOptions::dropMacros << elem.text();
                    }
                    macro = macro.nextSibling();
                }
            }
            node = node.nextSibling();
        }
    } else {
        qWarning() << "Couldn't find config file" << configFile.filePath();
    }

    // first try to load plugins from the executable's directory
    QLibrary lib(app.applicationDirPath() + "/generator_" + generator);
    lib.load();
    if (!lib.isLoaded()) {
        lib.unload();
        lib.setFileName(app.applicationDirPath() + "/../lib" + LIB_SUFFIX + "/smokegen/generator_" + generator);
        lib.load();
    }
    if (!lib.isLoaded()) {
        lib.unload();
        lib.setFileName("generator_" + generator);
        lib.load();
    }
    if (!lib.isLoaded()) {
        qCritical() << lib.errorString();
        return EXIT_FAILURE;
    }
    qDebug() << "using generator" << lib.fileName();
    GenerateFn generate = (GenerateFn) lib.resolve("generate");
    if (!generate) {
        qCritical() << "couldn't resolve symbol 'generate', aborting";
        return EXIT_FAILURE;
    }
    
    foreach (QDir dir, ParserOptions::includeDirs) {
        if (!dir.exists()) {
            qWarning() << "include directory" << dir.path() << "doesn't exist";
        }
    }
    // Filter out non-existent directories without modifying while iterating
    QList<QDir> validDirs;
    qDebug() << "DEBUG: Starting validation loop, initial count =" << ParserOptions::includeDirs.size();
    foreach (QDir dir, ParserOptions::includeDirs) {
        QString p = dir.path();
        if (p.isEmpty()) {
            qDebug() << "DEBUG: Skipping empty include dir entry";
            continue;
        }
        if (dir.exists()) {
            validDirs << dir;
            qDebug() << "DEBUG: Added valid dir:" << p;
        } else {
            qDebug() << "DEBUG: Skipped invalid dir:" << p;
        }
    }
    qDebug() << "DEBUG: Validation complete, validDirs count =" << validDirs.size();
    ParserOptions::includeDirs = validDirs;
    qDebug() << "DEBUG: Reassignment complete, ParserOptions::includeDirs count =" << ParserOptions::includeDirs.size();
    
    QStringList defines;
    if (ParserOptions::definesList.exists()) {
        QFile file(ParserOptions::definesList.filePath());
        file.open(QIODevice::ReadOnly);
        while (!file.atEnd()) {
            QByteArray array = file.readLine();
            if (!array.isEmpty())
                defines << array.trimmed();
        }
        file.close();
    } else if (!ParserOptions::definesList.filePath().isEmpty()) {
        qWarning() << "didn't find file" << ParserOptions::definesList.filePath();
    }
    
    QFile log("generator.log");
    bool logErrors = log.open(QFile::WriteOnly | QFile::Truncate);
    QTextStream logOut(&log);
    
    qDebug() << "Hello from smokegen!  AND IM UPDATED";

    foreach (QFileInfo file, ParserOptions::headerList) {
        qDebug() << "parsing" << file.absoluteFilePath();

#if defined(_WIN32)
        __try {
#endif
        // Build argument list for this file
        std::vector<std::string> fileArgv;
        fileArgv.push_back(std::string(app.applicationFilePath().toStdString()));
        fileArgv.push_back("-x");
        fileArgv.push_back("c++");
        
        foreach (QDir dir, ParserOptions::includeDirs) {
            QString p = dir.path();
            qDebug() << "adding include directory" << p;
            if (p.isEmpty()) {
                qDebug() << "DEBUG: Skipping empty include dir during argv build";
                continue;
            }
            std::string dirPath = p.toStdString();
            qDebug() << "DEBUG: Got dir path string OK";
            std::string dirStr;
            dirStr = "-I";
            dirStr += dirPath;
            qDebug() << "DEBUG: Constructed dirStr OK";
            fileArgv.push_back(dirStr);
            qDebug() << "DEBUG: Pushed dirStr to fileArgv OK";
        }
        qDebug() << "DEBUG: Done with include directories";
        foreach (QDir dir, ParserOptions::frameworkDirs) {
            qDebug() << "DEBUG: Adding framework dir";
            fileArgv.push_back("-iframework");
            std::string fwkStr = dir.path().toStdString();
            fileArgv.push_back(fwkStr);
        }
        qDebug() << "DEBUG: Done with framework dirs";
        
        foreach (QString define, defines) {
            qDebug() << "DEBUG: Adding define";
            std::string defStr = "-D" + define.toStdString();
            fileArgv.push_back(defStr);
        }
        qDebug() << "DEBUG: Done with defines";
        
        // Add clang options that were passed on command line
        if (!clangOptions.empty()) {
            qDebug() << "DEBUG: clangOptions count=" << (int)clangOptions.size();
            // Reserve to avoid reallocation during push_back which could
            // cause issues if vector's allocator/state differs across libs.
            fileArgv.reserve(fileArgv.size() + clangOptions.size());
            for (const auto& opt : clangOptions) {
                qDebug() << "DEBUG: Adding clang option";
                fileArgv.push_back(opt);
            }
        } else {
            qDebug() << "DEBUG: No clang options to add";
        }
        qDebug() << "DEBUG: Done with clang options";
        
        qDebug() << "DEBUG: About to add file path";
        std::string fileStr = file.absoluteFilePath().toStdString();
        // Ensure there's space before pushing the file path
        fileArgv.reserve(fileArgv.size() + 2);
        fileArgv.push_back(fileStr);
        qDebug() << "DEBUG: File path added";
        
        fileArgv.push_back("-I/builtins");
        qDebug() << "DEBUG: Added -I/builtins";
        
        fileArgv.push_back("-fsyntax-only");
        qDebug() << "DEBUG: Added -fsyntax-only";

        qDebug() << "DEBUG: About to create FileManager";

        // Create FileManager with default filesystem
        clang::FileSystemOptions fsOptions;
        clang::FileManager FM(fsOptions, llvm::vfs::getRealFileSystem());

        std::cerr << "DEBUG: FileManager created successfully\n" << std::flush;

        // Skip embedded files for now - just pass -I/builtins on command line
        // The crash appears to be in the embedded files loop itself

        // Use the std::unique_ptr<FrontendAction> overload for LLVM 19
        std::cerr << "DEBUG: About to create ToolInvocation\n" << std::flush;
        clang::tooling::ToolInvocation inv(fileArgv, std::make_unique<SmokegenFrontendAction>(), &FM, std::make_shared<clang::PCHContainerOperations>());

        std::cerr << "DEBUG: ToolInvocation created successfully\n" << std::flush;
        qDebug() << "About to run inv";

        if (!inv.run()) {
            qDebug() << "parsing of" << file.absoluteFilePath() << "failed";
            qCritical() << "Continuing to next file despite parse failure";
        } else {
            // Parsing succeeded - let clang's objects destruct naturally
            qDebug() << "inv.run() returned successfully";
            qDebug() << "parsing of" << file.absoluteFilePath() << "succeeded";
        }

#if defined(_WIN32)
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            DWORD code = GetExceptionCode();
            qCritical() << "SEH: exception code:" << QString::number(code, 16);
            qCritical() << "Crash occurred while processing:" << file.absoluteFilePath();
            std::cerr << "SEH: exception code: 0x" << std::hex << code << std::dec << "\n" << std::flush;
            std::cerr << "Crash while parsing: " << file.absoluteFilePath().toStdString() << "\n" << std::flush;
            qWarning() << "Continuing to next file after crash";
        }
#endif
        // this has already been parsed because it was included by some header
        if (!logErrors)
            continue;
    }
    
    log.close();
    qDebug() << "generation log written to generator.log";
    return generate();
}
