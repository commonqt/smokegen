#include <regex>
#include <iostream>

#include <clang/AST/ASTContext.h>
#include <clang/Basic/Version.h>

#include "astvisitor.h"
#include "defaultargvisitor.h"

bool SmokegenASTVisitor::VisitCXXRecordDecl(clang::CXXRecordDecl *D) {
    registerClass(D);

    return true;
}

bool SmokegenASTVisitor::VisitEnumDecl(clang::EnumDecl *D) {
    registerEnum(D);

    return true;
}

bool SmokegenASTVisitor::VisitFunctionDecl(clang::FunctionDecl *D) {
    if (clang::isa<clang::CXXMethodDecl>(D)){
        return true;
    }

    if (!D->getDeclName())
        return true;

    if (D->isDependentContext() || D->getTemplateSpecializationInfo()) {
        return true;
    }

    // Skip functions that use va_args
    for (const clang::ParmVarDecl* parm : D->parameters()) {
        clang::QualType t = parm->getType();
        while (t->isPointerType()) {
            t = t->getPointeeType();
        }
        t = t.getCanonicalType();

#if CLANG_VERSION_MAJOR > 3 || CLANG_VERSION_MINOR > 7
        clang::RecordDecl* vaListTagDecl = (clang::RecordDecl*)ci.getASTContext().getVaListTagDecl();
        if (vaListTagDecl && t == ci.getASTContext().getRecordType(vaListTagDecl)) {
#else
        if (t == ci.getASTContext().getVaListTagType()) {
#endif
            return true;
        }
    }
    registerFunction(D);

    return true;
}

bool SmokegenASTVisitor::VisitTypedefNameDecl(clang::TypedefNameDecl *D) {

    registerTypedef(D);

    return true;
}

clang::QualType SmokegenASTVisitor::getReturnTypeForFunction(const clang::FunctionDecl* function) const {
    if (const auto ctor = clang::dyn_cast<clang::CXXConstructorDecl>(function)) {
        return ci.getASTContext().getPointerType(clang::QualType(ctor->getParent()->getTypeForDecl(), 0));
    }
    else {
        return function->getReturnType();
    }
}

Access SmokegenASTVisitor::toAccess(clang::AccessSpecifier clangAccess) const {
    Access access;
    switch (clangAccess) {
        case clang::AS_public:
        case clang::AS_none:
            access = Access_public;
            break;
        case clang::AS_protected:
            access = Access_protected;
            break;
        case clang::AS_private:
            access = Access_private;
            break;
    }
    return access;
}

Parameter SmokegenASTVisitor::toParameter(const clang::ParmVarDecl* param) const {
    Type* paramType = registerType(param->getType());
    if (paramType->getTypedef()) {
        paramType = typeFromTypedef(paramType->getTypedef(), paramType);
    }
    Parameter parameter(
        QString::fromStdString(param->getNameAsString()),
        paramType
    );

    

    if (const clang::Expr* defaultArgExpr = (param->hasUninstantiatedDefaultArg() ? param->getUninstantiatedDefaultArg() : param->getDefaultArg())) {
        std::string defaultArgStr;
        llvm::raw_string_ostream s(defaultArgStr);
        defaultArgExpr->printPretty(s, nullptr, pp());
        parameter.setDefaultValue(QString::fromStdString(s.str()));

        DefaultArgVisitor argVisitor(ci);
        argVisitor.TraverseStmt(const_cast<clang::Expr*>(defaultArgExpr));
        std::string resolved = argVisitor.toString(defaultArgExpr);
        if (!resolved.empty()) {
            QString resolvedQString = QString::fromStdString(resolved);
            resolvedQString = resolvedQString.replace(QRegExp("^=[\\s]*"), "");
            parameter.setDefaultValue(resolvedQString);
        }
    }

    return parameter;
}

Class* SmokegenASTVisitor::registerClass(const clang::CXXRecordDecl* clangClass) const {
    // We can't make bindings for things that don't have names.
    if (!clangClass->getDeclName())
        return nullptr;

    clang::PresumedLoc ploc = ci.getSourceManager().getPresumedLoc(clangClass->getSourceRange().getBegin());
    if (!ploc.isValid())
        return nullptr;

    clangClass = clangClass->hasDefinition() ? clangClass->getDefinition() : clangClass->getCanonicalDecl();

    QString qualifiedName = QString::fromStdString(clangClass->getQualifiedNameAsString());
    if (classes.contains(qualifiedName) && !classes[qualifiedName].isForwardDecl()) {
        // We already have this class
        return &classes[qualifiedName];
    }

    QString name = QString::fromStdString(clangClass->getNameAsString());

    QString nspace;
    Class* parent = nullptr;
    if (const auto clangParent = clang::dyn_cast<clang::NamespaceDecl>(clangClass->getParent())) {
        nspace = QString::fromStdString(clangParent->getQualifiedNameAsString());
    }
    else if (const auto clangParent = clang::dyn_cast<clang::CXXRecordDecl>(clangClass->getParent())) {
        parent = registerClass(clangParent);
    }
    Class::Kind kind;
    switch (clangClass->getTagKind()) {
        case clang::TTK_Class:
            kind = Class::Kind_Class;
            break;
        case clang::TTK_Struct:
            kind = Class::Kind_Struct;
            break;
        case clang::TTK_Union:
            kind = Class::Kind_Union;
            break;
        default:
            break;
    }

    bool isForward = !clangClass->hasDefinition();

    Class localClass(name, nspace, parent, kind, isForward);
    classes[qualifiedName] = localClass;
    Class* klass = &classes[qualifiedName];

    klass->setAccess(toAccess(clangClass->getAccess()));
    klass->setFileName(QString(ploc.getFilename()));

    if (clangClass->getTypeForDecl()->isDependentType() || clang::isa<clang::ClassTemplateSpecializationDecl>(clangClass)) {
        klass->setIsTemplate(true);
    }

    if (!isForward) {
        if (!clangClass->getTypeForDecl()->isDependentType()) {
            addQPropertyAnnotations(clangClass);

            // Set base classes
            for (const clang::CXXBaseSpecifier& base : clangClass->bases()) {
                const clang::CXXRecordDecl* baseRecordDecl = base.getType()->getAsCXXRecordDecl();

                if (!baseRecordDecl) {
                    // Ignore template specializations
                    continue;
                }

                Class::BaseClassSpecifier baseClass = Class::BaseClassSpecifier{
                    &classes[QString::fromStdString(baseRecordDecl->getQualifiedNameAsString())],
                    toAccess(base.getAccessSpecifier()),
                    base.isVirtual()
                };

                klass->appendBaseClass(baseClass);
            }
        }

        // Set methods
        QList<const clang::CXXMethodDecl*> methods;

        for (auto method : clangClass->methods())
            methods.append(method);

        for (const clang::CXXMethodDecl* method : methods) {
            if (method->isImplicit()) {
                continue;
            }

            clang::QualType clangReturnType = getReturnTypeForFunction(method);

            if (klass->isTemplate() && clang::dyn_cast<clang::TemplateSpecializationType>(clangReturnType))
                continue;

            Type* returnType = registerType(clangReturnType);
            if (returnType->getTypedef()) {
                returnType = typeFromTypedef(returnType->getTypedef(), returnType);
            }
            Method newMethod = Method(
                klass,
                QString::fromStdString(method->getNameAsString()),
                returnType,
                method->isDeleted() ? Access_private : toAccess(method->getAccess())
            );


            newMethod.setIsDeleted(method->isDeleted());


            // Avoid collecting methods we do not know how to call it.
            // We need to collect some information about template classes but... take it easy...
            if (klass->isTemplate() && newMethod.access() != Access_private)
                continue;

            for (auto attr_it = method->specific_attr_begin<clang::AnnotateAttr>();
              attr_it != method->specific_attr_end<clang::AnnotateAttr>();
              ++attr_it) {
                const clang::AnnotateAttr *A = *attr_it;
                if (A->getAnnotation() == "qt_signal") {
                    newMethod.setIsSignal(true);
                }
                else if (A->getAnnotation() == "qt_slot") {
                    newMethod.setIsSlot(true);
                }
                if (A->getAnnotation() == "qt_property") {
                    newMethod.setIsQPropertyAccessor(true);
                }
            }
            if (const clang::CXXConversionDecl* conversion = clang::dyn_cast<clang::CXXConversionDecl>(method)) {
                newMethod.setName(QString::fromStdString("operator " + conversion->getConversionType().getAsString(pp())));
            }

            if (const clang::CXXConstructorDecl* ctor = clang::dyn_cast<clang::CXXConstructorDecl>(method)) {
                // if (!ctor->isDeleted() && clangClass->isAbstract()) continue;
                newMethod.setIsConstructor(true);
                if (ctor->getExplicitSpecifier().isExplicit()) {
                    newMethod.setFlag(Member::Explicit);
                }
            }
            else if (clang::isa<clang::CXXDestructorDecl>(method)) {
                newMethod.setIsDestructor(true);
            }
            newMethod.setIsConst(method->isConst());
            if (method->isVirtual()) {
                newMethod.setFlag(Member::Virtual);
                if (method->isPure()) {
                    newMethod.setFlag(Member::PureVirtual);
                }
            }
            if (method->isStatic()) {
                newMethod.setFlag(Member::Static);
            }

            bool foundNotCompatibleParameter = false;
            for (const clang::ParmVarDecl* param : method->parameters()) {
                if (klass->isTemplate() && clang::dyn_cast<clang::TemplateTypeParmType>(param->getType()))
                {
                    foundNotCompatibleParameter = true;
                    break;
                }

                // TODO handle RValue on functions xpto(type &&s)
                if (clang::dyn_cast<clang::RValueReferenceType>(param->getType()))
                {
                    foundNotCompatibleParameter = true;
                    break;
                }
                newMethod.appendParameter(toParameter(param));
            }

            if (foundNotCompatibleParameter)
                continue;

            klass->appendMethod(newMethod, true);
        }

        for (const clang::Decl* decl : clangClass->decls()) {
            const clang::VarDecl* varDecl = clang::dyn_cast<clang::VarDecl>(decl);
            const clang::FieldDecl* fieldDecl = clang::dyn_cast<clang::FieldDecl>(decl);
            if (!varDecl && !fieldDecl) {
                continue;
            }
            const clang::DeclaratorDecl* declaratorDecl = clang::dyn_cast<clang::DeclaratorDecl>(decl);
            Type* fieldType = registerType(declaratorDecl->getType());

            if (fieldType->getTypedef()) {
                fieldType = typeFromTypedef(fieldType->getTypedef(), fieldType);
            }
            if (!fieldType->isValid()) {
                continue;
            }
            Field field(
                klass,
                QString::fromStdString(declaratorDecl->getNameAsString()),
                fieldType,
                toAccess(declaratorDecl->getAccess())
            );
            if (varDecl) {
                field.setFlag(Member::Static);
            }
            klass->appendField(field);
        }
    }
    return klass;
}

Enum* SmokegenASTVisitor::registerEnum(const clang::EnumDecl* clangEnum) const {
    clangEnum = clangEnum->getDefinition();
    if (!clangEnum) {
        return nullptr;
    }

    QString qualifiedName = QString::fromStdString(clangEnum->getQualifiedNameAsString());
    if (enums.contains(qualifiedName)) {
        // We already have this class
        return &enums[qualifiedName];
    }

    QString name = QString::fromStdString(clangEnum->getNameAsString());
    QString nspace;
    Class* parent = nullptr;
    if (const auto clangParent = clang::dyn_cast<clang::NamespaceDecl>(clangEnum->getParent())) {
        nspace = QString::fromStdString(clangParent->getQualifiedNameAsString());
    }
    else if (const auto clangParent = clang::dyn_cast<clang::CXXRecordDecl>(clangEnum->getParent())) {
        parent = registerClass(clangParent);
    }

    Enum localE(
        name,
        nspace,
        parent
    );

    enums[qualifiedName] = localE;
    Enum* e = &enums[qualifiedName];
    e->setAccess(toAccess(clangEnum->getAccess()));

    if (parent) {
        parent->appendChild(e);
    }

    for (const clang::EnumConstantDecl* enumVal : clangEnum->enumerators()) {
        
        EnumMember member(
            e,
            QString::fromStdString(clangEnum->isScoped() ? name.toStdString() + "::" + enumVal->getNameAsString() : enumVal->getNameAsString())
        );
        // The existing parser doesn't set the values for enums.
        //if (const clang::Expr* initExpr = enumVal->getInitExpr()) {
        //    std::string initExprStr;
        //    llvm::raw_string_ostream s(initExprStr);
        //    initExpr->printPretty(s, nullptr, pp());
        //    member.setValue(QString::fromStdString(s.str()));
        //}
        e->appendMember(member);
    }
    return e;
}

Function* SmokegenASTVisitor::registerFunction(const clang::FunctionDecl* clangFunction) const {
    clangFunction = clangFunction->getCanonicalDecl();

    std::string signatureStr = clangFunction->getQualifiedNameAsString();
    clangFunction->getType().getAsStringInternal(signatureStr, pp());
    QString signature = QString::fromStdString(signatureStr);

    if (functions.contains(signature)) {
        // We already have this function
        return &functions[signature];
    }

    QString name = QString::fromStdString(clangFunction->getNameAsString());
    QString nspace;
    if (const auto clangParent = clang::dyn_cast<clang::NamespaceDecl>(clangFunction->getParent())) {
        nspace = QString::fromStdString(clangParent->getQualifiedNameAsString());
    }

    Type* returnType = registerType(getReturnTypeForFunction(clangFunction));
    if (returnType->getTypedef()) {
        returnType = typeFromTypedef(returnType->getTypedef(), returnType);
    }
    Function newFunction(
        name,
        nspace,
        returnType
    );

    for (const clang::ParmVarDecl* param : clangFunction->parameters()) {
        newFunction.appendParameter(toParameter(param));
    }
    clang::PresumedLoc ploc = ci.getSourceManager().getPresumedLoc(clangFunction->getSourceRange().getBegin());
    if (ploc.isValid()) {
        newFunction.setFileName(QString(ploc.getFilename()));
    }

    functions[signature] = newFunction;
    return &functions[signature];
}

Type* SmokegenASTVisitor::registerType(clang::QualType clangType) const {
    clang::QualType orig = clang::QualType(clangType);

    Type type;

    if (clangType->isReferenceType()) {
        type.setIsRef(true);

        clangType = clangType->getPointeeType();
    }
    clang::QualType prevType = clangType;
    while (clangType->isPointerType()) {
        if (clangType->isFunctionPointerType()) {
            type.setIsFunctionPointer(true);
            const clang::FunctionType* fnType = clangType->getPointeeType()->getAs<clang::FunctionType>();
            clangType = fnType->getReturnType();

            if (const clang::FunctionProtoType* proto = clang::dyn_cast<clang::FunctionProtoType>(fnType)) {
                for (const clang::QualType param : proto->param_types()) {
                    type.appendParameter(Parameter("", registerType(param)));
                }
            }

            if (clangType->isReferenceType()) {
                type.setIsRef(true);
                clangType = clangType->getPointeeType();
            }
        }
        else {
            type.setPointerDepth(type.pointerDepth() + 1);

            clangType = clangType->getPointeeType();

            if (type.pointerDepth() > 1) {
                if (prevType.isConstQualified()) {
                    // type.isConst is used if the first pointer depth type is
                    // const. isConstPointer referrs to things farther down.
                    type.setIsConstPointer(type.pointerDepth() - 2, true);
                }
            }
        }
        prevType = clangType;
    }

    while (const clang::ConstantArrayType* arrayType = clang::dyn_cast<clang::ConstantArrayType>(clangType)) {
        type.setArrayDimensions(type.arrayDimensions() + 1);
        type.setArrayLength(type.arrayDimensions() - 1, arrayType->getSize().getLimitedValue());
        clangType = arrayType->getElementType();
    }

    type.setIsConst(clangType.isConstQualified());
    type.setIsVolatile(clangType.isVolatileQualified());

    // We've got all the qualifier info we need.  Remove it so that the
    // qualifiers don't appear in the type name.
    clangType = clangType.getUnqualifiedType();

    type.setIsIntegral(clangType->isBuiltinType());
    type.setName(QString::fromStdString(clangType.getAsString(pp())));

    // According to the clang docs, elaborated types:
    // Represents a type that was referred to using an elaborated type keyword,
    // e.g., struct S, or via a qualified name, e.g., N::M::type, or both.
    // This type is used to keep track of a type name as written in the source
    // code, including tag keywords and any nested-name-specifiers. The type
    // itself is always "sugar", used to express what was written in the source
    // code but containing no additional semantic information.
    // This specifically comes up in QtGui, that makes use of the HANDLE
    // typedef defined in the Qt namespace, and referred to via Qt::HANDLE
    if (const clang::ElaboratedType* elaboratedType = clang::dyn_cast<clang::ElaboratedType>(clangType)) {
        clangType = elaboratedType->getNamedType();
    }

    if (clangType->isRecordType() && !clangType->castAs<clang::RecordType>()->getDecl()->getIdentifier()) {
        type.setName(""); // Makes the type invalid.  Don't set typedef or class.
    }
    else if (const clang::TypedefType* typedefType = clang::dyn_cast<clang::TypedefType>(clangType)) {
        clang::TypedefNameDecl* typedefDecl = typedefType->getDecl();
        if (type.name().toStdString() != typedefDecl->getUnderlyingType().getCanonicalType().getAsString(pp())) {
            type.setTypedef(registerTypedef(typedefDecl));
        }
    }
    else if (const clang::CXXRecordDecl* clangClass = clangType->getAsCXXRecordDecl()) {
        type.setClass(registerClass(clangClass));

        const auto templateSpecializationDecl = clang::dyn_cast<clang::ClassTemplateSpecializationDecl>(clangClass);
        if (templateSpecializationDecl) {
            const auto & args = templateSpecializationDecl->getTemplateArgs();
            for (int i=0; i < args.size(); ++i) {
                switch (args[i].getKind()) {
                    case clang::TemplateArgument::Integral:
                    {
                        Type tempArgType;
                        clang::QualType clangTempArgType = args[i].getIntegralType();
                        if (const clang::EnumType* e = clang::dyn_cast<clang::EnumType>(clangTempArgType)) {
                            // Handle templates specializations based on enum
                            // values.  Clang identifies these as an integral
                            // type.  Find the enum constant that corresponds
                            // to this template argument value
                            for (const clang::EnumConstantDecl* enumConstant : e->getDecl()->enumerators()) {
                                if (enumConstant->getInitVal() == args[i].getAsIntegral()) {
                                    tempArgType.setName(QString::fromStdString(
                                        clang::cast<clang::NamedDecl>(enumConstant->getDeclContext()->getParent())->getQualifiedNameAsString() + "::" +
                                        enumConstant->getNameAsString()
                                    ));
                                    break;
                                }
                            }
                        }
                        if (tempArgType.name().isEmpty()) {
                            tempArgType.setName(QString::fromStdString(args[i].getAsIntegral().toString(10)));
                        }
                        type.appendTemplateArgument(tempArgType);
                        break;
                    }
                    case clang::TemplateArgument::Type:
                    {
                        clang::QualType templateType = args[i].getAsType();
                        type.appendTemplateArgument(*registerType(templateType));
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    }
    else if (const clang::EnumDecl* clangEnum = clang::dyn_cast_or_null<clang::EnumDecl>(clangType->getAsTagDecl())) {
        type.setEnum(registerEnum(clangEnum));
    }
    return Type::registerType(type);
}

Typedef* SmokegenASTVisitor::registerTypedef(const clang::TypedefNameDecl* clangTypedef) const {
    clangTypedef = clangTypedef->getCanonicalDecl();

    QString qualifiedName = QString::fromStdString(clangTypedef->getQualifiedNameAsString());
    if (typedefs.contains(qualifiedName)) {
        // We already have this typedef
        return &typedefs[qualifiedName];
    }
    if (clangTypedef->getUnderlyingType().getCanonicalType()->isDependentType()) {
        return nullptr;
    }

    QString name = QString::fromStdString(clangTypedef->getNameAsString());
    QString nspace;
    Class* parent = nullptr;
    if (const auto clangParent = clang::dyn_cast_or_null<clang::NamespaceDecl>(clangTypedef->getDeclContext())) {
        nspace = QString::fromStdString(clangParent->getQualifiedNameAsString());
    }
    else if (const auto clangParent = clang::dyn_cast_or_null<clang::CXXRecordDecl>(clangTypedef->getDeclContext())) {
        parent = registerClass(clangParent);
    }

    Typedef tdef(
        registerType(clangTypedef->getUnderlyingType().getCanonicalType()),
        name,
        nspace,
        parent
    );

    typedefs[qualifiedName] = tdef;
    return &typedefs[qualifiedName];
}

Type* SmokegenASTVisitor::typeFromTypedef(const Typedef* tdef, const Type* sourceType) const {
    Type targetType = tdef->resolve();
    targetType.setIsRef(sourceType->isRef());
    targetType.setIsConst(sourceType->isConst());
    targetType.setIsVolatile(sourceType->isVolatile());
    targetType.setPointerDepth(targetType.pointerDepth() + sourceType->pointerDepth());
    for (int i = 0; i < sourceType->pointerDepth(); i++) {
        targetType.setIsConstPointer(i, sourceType->isConstPointer(i));
    }
    targetType.setIsFunctionPointer(sourceType->isFunctionPointer());
    for (int i = 0; i < sourceType->parameters().size(); i++) {
        targetType.appendParameter(sourceType->parameters()[i]);
    }
    targetType.setArrayDimensions(sourceType->arrayDimensions());
    for (int i = 0; i < sourceType->arrayDimensions(); i++) {
        targetType.setArrayLength(i, sourceType->arrayLength(i));
    }
    return Type::registerType(targetType);
}

void SmokegenASTVisitor::addQPropertyAnnotations(const clang::CXXRecordDecl* D) const {
    clang::ASTContext* ctx = &ci.getASTContext();
    for (const auto& d : D->decls()) {
        if (clang::StaticAssertDecl *S = llvm::dyn_cast<clang::StaticAssertDecl>(d) ) {
            if (auto *E = llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(S->getAssertExpr())) {
                if (clang::ParenExpr *PE = llvm::dyn_cast<clang::ParenExpr>(E->getArgumentExpr())) {
                    llvm::StringRef key = S->getMessage()->getString();
                    if (key == "qt_property") {
                        clang::StringLiteral *Val = llvm::dyn_cast<clang::StringLiteral>(PE->getSubExpr());

                        std::string propertyStr = Val->getString().str();
                        std::smatch match;
                        std::regex readRe("READ +([^ ]*)");

                        if (std::regex_search(propertyStr, match, readRe)) {
                            auto Name = ctx->DeclarationNames.getIdentifier(&ctx->Idents.get(llvm::StringRef(match[1])));
                            auto lookup = D->lookup(Name);
                            for (clang::NamedDecl* namedDecl : lookup) {
                                if (clang::CXXMethodDecl* method = clang::dyn_cast<clang::CXXMethodDecl>(namedDecl)) {
                                    auto annotate = clang::AnnotateAttr(*ctx, clang::AttributeCommonInfo(clang::SourceRange()), llvm::StringRef("qt_property")).clone(*ctx);
                                    method->addAttr(annotate);
                                }
                            }
                        }

                        std::regex writeRe("WRITE +([^ ]*)");
                        if (std::regex_search(propertyStr, match, writeRe)) {
                            auto Name = ctx->DeclarationNames.getIdentifier(&ctx->Idents.get(llvm::StringRef(match[1])));
                            auto lookup = D->lookup(Name);
                            for (clang::NamedDecl* namedDecl : lookup) {
                                if (clang::CXXMethodDecl* method = clang::dyn_cast<clang::CXXMethodDecl>(namedDecl)) {
                                    auto annotate = clang::AnnotateAttr(*ctx, clang::AttributeCommonInfo(clang::SourceRange()), llvm::StringRef("qt_property")).clone(*ctx);
                                    method->addAttr(annotate);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
