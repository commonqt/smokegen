/*
    Generator for the SMOKE sources
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
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QString>
#include <QtDebug>

#include <type.h>

QMap<QString, int> classIndex;
QDir outputDir;
QList<QFileInfo> headerList;
QStringList classList;

int parts = 20;
QString module = "qt";

QSet<Class*> externClasses;

QHash<const Class*, QList<const Class*> > superClassCache;
QHash<const Class*, QList<const Class*> > descendantsClassCache;

void writeClassFiles();
void writeSmokeData();

extern "C" Q_DECL_EXPORT
void generate(const QDir& outputDir, const QList<QFileInfo>& headerList, const QStringList& classes)
{
    ::outputDir = outputDir;
    ::headerList = headerList;
    classList = classes;
    int i = 1;
    
    // build table classname => index
    for (QHash<QString, Class>::const_iterator iter = ::classes.constBegin(); iter != ::classes.constEnd(); iter++) {
        if (classList.contains(iter.key()) && !iter.value().isForwardDecl())
            classIndex[iter.key()] = 1;
    }
    
    for (QMap<QString, int>::iterator iter = classIndex.begin(); iter != classIndex.end(); iter++)
        iter.value() = i++;
    
    writeClassFiles();
    writeSmokeData();
}

QList<const Class*> superClassList(const Class* klass)
{
    QList<const Class*> ret;
    if (superClassCache.contains(klass))
        return superClassCache[klass];
    foreach (const Class::BaseClassSpecifier& base, klass->baseClasses()) {
        ret << base.baseClass;
        ret.append(superClassList(base.baseClass));
    }
    // cache
    superClassCache[klass] = ret;
    return ret;
}

QList<const Class*> descendantsList(const Class* klass)
{
    QList<const Class*> ret;
    if (descendantsClassCache.contains(klass))
        return descendantsClassCache[klass];
    for (QHash<QString, Class>::const_iterator iter = classes.constBegin(); iter != classes.constEnd(); iter++) {
        if (superClassList(&iter.value()).contains(klass))
            ret << &iter.value();
    }
    // cache
    descendantsClassCache[klass] = ret;
    return ret;
}

void writeClass(QTextStream& out, const Class* klass);

void writeClassFiles()
{
    // how many classes go in one file
    int count = classIndex.count() / parts;
    int count2 = count;
    
    QList<QString> keys = classIndex.keys();
    
    for (int i = 0; i < parts; i++) {
        QSet<QString> includes;
        QString classCode;
        QTextStream classOut(&classCode);
        
        // write the class code to a QString so we can later prepend the #includes
        if (i == parts - 1) count2 = -1;
        foreach (const QString& str, keys.mid(count * i, count2)) {
            const Class* klass = &classes[str];
            includes.insert(klass->fileName());
            writeClass(classOut, klass);
        }
        
        // create the file
        QFile file(outputDir.filePath("x_" + QString::number(i + 1) + ".cpp"));
        file.open(QFile::ReadWrite | QFile::Truncate);

        QTextStream fileOut(&file);
        
        // write out the header
        fileOut << "//Auto-generated by " << QCoreApplication::arguments()[0] << ". DO NOT EDIT.\n";
        fileOut << "#include <smoke.h>\n#include <" << module << "_smoke.h>\n";

        // ... and the #includes
        QList<QString> sortedIncludes = includes.toList();
        qSort(sortedIncludes.begin(), sortedIncludes.end());
        foreach (const QString& str, sortedIncludes) {
            fileOut << "#include <" << str << ">\n";
        }

        // now the class code
        fileOut << "\n" << classCode;
        
        file.close();
    }
}

void writeClass(QTextStream& out, const Class* klass)
{
    const QString className = klass->toString();
    const QString smokeClassName = QString(className).replace("::", "__");
    
    out << QString("class x_%1 : public %2 {\n").arg(smokeClassName).arg(className);
    out << "    SmokeBinding* _binding;\n";
    out << "}\n\n";
}

void writeSmokeData()
{
    QFile smokedata(outputDir.filePath("smokedata.cpp"));
    smokedata.open(QFile::ReadWrite | QFile::Truncate);
    QTextStream out(&smokedata);
    foreach (const QFileInfo& file, headerList)
        out << "#include <" << file.fileName() << ">\n";
    out << "\n#include <smoke.h>\n";
    out << "#include <" << module << "_smoke.h>\n\n";
    
    // write out module_cast() function
    out << "static void *" << module << "_cast(void *xptr, Smoke::Index from, Smoke::Index to) {\n";
    out << "  switch(from) {\n";
    for (QMap<QString, int>::const_iterator iter = classIndex.constBegin(); iter != classIndex.constEnd(); iter++) {
        out << "    case " << iter.value() << ":   //" << iter.key() << "\n";
        out << "      switch(to) {\n";
        const Class& klass = classes[iter.key()];
        foreach (const Class* base, superClassList(&klass)) {
            QString className = base->toString();
            out << QString("        case %1: return (void*)(%2*)(%3*)xptr;\n")
                .arg(classIndex[className]).arg(className).arg(klass.toString());
        }
        out << QString("        case %1: return (void*)(%2*)xptr;\n").arg(iter.value()).arg(klass.toString());
        foreach (const Class* desc, descendantsList(&klass)) {
            QString className = desc->toString();
            out << QString("        case %1: return (void*)(%2*)(%3*)xptr;\n")
                .arg(classIndex[className]).arg(className).arg(klass.toString());
        }
        out << "        default: return xptr;\n";
        out << "      }\n";
    }
    out << "    default: return xptr;\n";
    out << "  }\n";
    out << "}\n\n";
    
    // write out the inheritance list
    out << "// Group of Indexes (0 separated) used as super class lists.\n";
    out << "// Classes with super classes have an index into this array.\n";
    out << "static Smoke::Index " << module << "_inheritanceList[] = {\n";
    out << "    0,\t// 0: (no super class)\n";
    out << "}\n\n";
    smokedata.close();
}