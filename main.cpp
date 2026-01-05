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
# include <crtdbg.h>

// Custom assertion handler to prevent dialog boxes
int customAssertHandler(int reportType, char* message, int* returnValue) {
    // Log the assertion to stderr instead of showing a dialog
    std::cerr << "CRT Assertion: " << message << std::endl;
    // Return 1 to tell CRT to continue execution (suppress dialog)
    *returnValue = 0;
    return 1; // TRUE means we handled it
}
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
    try
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

    std::vector<std::string> Argv {
        argv[0],
        "-x", "c++",
    };

    for (int i = 1; i < args.count(); i++) {
        if ((args[i] == "-I" || args[i] == "-d" || args[i] == "-dm" ||
             args[i] == "-g" || args[i] == "-config") && i + 1 >= args.count())
        {
            qCritical() << "not enough parameters for option" << args[i];
            return EXIT_FAILURE;
        }
        if (args[i] == "-I") {
            ParserOptions::includeDirs << QDir(args[++i]);
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
            Argv.push_back(args[i].toStdString());
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
                        ParserOptions::includeDirs << QDir(elem.text());
                    }
                    else if (elem.tagName() == "framework") {
                        ParserOptions::frameworkDirs << QDir(elem.text());
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
            ParserOptions::includeDirs.removeAll(dir);
        }
    }
    
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
    
    foreach (QFileInfo file, ParserOptions::headerList) {
        qDebug() << "parsing" << file.absoluteFilePath();

        foreach (QDir dir, ParserOptions::includeDirs) {
            Argv.push_back("-I" + dir.path().toStdString());
        }
        foreach (QDir dir, ParserOptions::frameworkDirs) {
            Argv.push_back("-iframework");
            Argv.push_back(dir.path().toStdString());
        }
        foreach (QString define, defines) {
            Argv.push_back("-D" + define.toStdString());
        }
        Argv.push_back(file.absoluteFilePath().toStdString());
        Argv.push_back("-I/builtins");
        Argv.push_back("-fsyntax-only");

        // Create an overlay file system with embedded files. Allocate on the heap
        // and intentionally leak to avoid destructor-induced heap corruption.
        auto RealFS = llvm::vfs::getRealFileSystem();
        auto *InMemFS = new llvm::vfs::InMemoryFileSystem;
        const EmbeddedFile* f = EmbeddedFiles;
        while (f->filename) {
            InMemFS->addFile(f->filename, 0, llvm::MemoryBuffer::getMemBuffer({f->content, f->size}, "", false));
            ++f;
        }

        auto *OverlayFS = new llvm::vfs::OverlayFileSystem(RealFS);
        OverlayFS->pushOverlay(llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>(InMemFS));

        auto *FM = new clang::FileManager({"."}, llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>(OverlayFS));

        auto inv = std::make_unique<clang::tooling::ToolInvocation>(Argv, std::make_unique<SmokegenFrontendAction>(), FM);

        if (!inv->run()) {
            return 1;
        }

        inv.release(); // leak to skip teardown

        // this has already been parsed because it was included by some header
        if (!logErrors)
            continue;
    }
    
    log.close();
    
    // Call the generator to produce output files
    int result = generate();
    
    // Use std::_Exit to skip destructors and avoid heap corruption during LLVM cleanup.
    // This is a common pattern in LLVM-based tools where:
    // 1. LLVM is statically linked, creating separate heaps in each module
    // 2. Cleanup is expensive and unnecessary for a short-lived tool
    // 3. All important work (file generation) is already complete
    // Proper fix: rebuild LLVM with -DBUILD_SHARED_LIBS=ON
    std::_Exit(result);
    }
    catch (const std::exception& e)
    {
        std::cout << "An error occured: " <<  e.what() << std::endl;
        std::_Exit(1);
    }
}
