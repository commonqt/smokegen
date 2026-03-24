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

#ifndef TYPE_H
#define TYPE_H

#include <QString>
#include <QStringList>
#include <QHash>
#include <QtDebug>
#include <memory>
#include <QList>

#include "generator_export.h"

class Class;
class Typedef;
class Enum;
class GlobalVar;
class Function;
class Type;

template<typename T>
class StableHashMap {
public:
    using Storage = QHash<QString, std::shared_ptr<T>>;
    using RawIterator = typename Storage::iterator;
    using RawConstIterator = typename Storage::const_iterator;

    class InternalIterator {
    public:
        InternalIterator() {}
        InternalIterator(const RawIterator& it) : m_it(it) {}
        bool operator!=(const InternalIterator& other) const { return m_it != other.m_it; }
        bool operator==(const InternalIterator& other) const { return m_it == other.m_it; }
        InternalIterator& operator++() { ++m_it; return *this; }
        QString key() const { return m_it.key(); }
        T& value() const { return *m_it.value(); }
    private:
        RawIterator m_it;
    };

    class InternalConstIterator {
    public:
        InternalConstIterator() {}
        InternalConstIterator(const RawConstIterator& it) : m_it(it) {}
        bool operator!=(const InternalConstIterator& other) const { return m_it != other.m_it; }
        bool operator==(const InternalConstIterator& other) const { return m_it == other.m_it; }
        InternalConstIterator& operator++() { ++m_it; return *this; }
        QString key() const { return m_it.key(); }
        const T& value() const { return *m_it.value(); }
    private:
        RawConstIterator m_it;
    };

    struct InsertResult {
        InternalIterator it;
        bool inserted;
        T& value() { return it.value(); }
    };

    StableHashMap() = default;
    ~StableHashMap() = default; // unique_ptr will clean up

    InsertResult insert(const QString& key, const T& value) {
        if (m_storage.contains(key)) {
            *m_storage[key] = value;
            RawIterator rawIt = m_storage.find(key);
            InternalIterator it(rawIt);
            return InsertResult{it, false};
        }
        m_storage.insert(key, std::make_shared<T>(value));
        RawIterator rawIt = m_storage.find(key);
        InternalIterator it(rawIt);
        return InsertResult{it, true};
    }

    bool contains(const QString& key) const { return m_storage.contains(key); }

    T& operator[](const QString& key) {
        if (!m_storage.contains(key)) {
            m_storage.insert(key, std::make_shared<T>());
        }
        return *m_storage[key];
    }

    const T& operator[](const QString& key) const { return *m_storage.value(key); }

    InternalIterator find(const QString& key) { return InternalIterator(m_storage.find(key)); }
    InternalConstIterator find(const QString& key) const { return InternalConstIterator(m_storage.find(key)); }

    InternalIterator begin() { return InternalIterator(m_storage.begin()); }
    InternalIterator end() { return InternalIterator(m_storage.end()); }
    InternalConstIterator begin() const { return InternalConstIterator(m_storage.constBegin()); }
    InternalConstIterator end() const { return InternalConstIterator(m_storage.constEnd()); }
    InternalConstIterator constBegin() const { return InternalConstIterator(m_storage.constBegin()); }
    InternalConstIterator constEnd() const { return InternalConstIterator(m_storage.constEnd()); }

    QList<T> values() const {
        QList<T> vals;
        for (auto it = m_storage.constBegin(); it != m_storage.constEnd(); ++it) {
            if (it.value()) vals.append(*it.value());
        }
        return vals;
    }

    QStringList keys() const {
        QStringList k;
        for (auto it = m_storage.constBegin(); it != m_storage.constEnd(); ++it) {
            k.append(it.key());
        }
        return k;
    }

    bool containsPointer(const T* p) const {
        if (!p) return false;
        for (auto it = m_storage.constBegin(); it != m_storage.constEnd(); ++it) {
            auto sp = it.value();
            if (!sp) continue;
            const T& v = *sp;
            if (&v == p) return true;
        }
        return false;
    }

private:
    Storage m_storage;
};

extern GENERATOR_EXPORT StableHashMap<Class> classes;
extern GENERATOR_EXPORT StableHashMap<Typedef> typedefs;
extern GENERATOR_EXPORT StableHashMap<Enum> enums;
extern GENERATOR_EXPORT StableHashMap<Function> functions;
extern GENERATOR_EXPORT StableHashMap<GlobalVar> globals;
extern GENERATOR_EXPORT StableHashMap<Type> types;

// Runtime validation helper: scans registries and prints inconsistencies when enabled
void validateAll();

class Method;
class Field;

enum Access {
    Access_public,
    Access_protected,
    Access_private
};

class Class;

class GENERATOR_EXPORT BasicTypeDeclaration
{
public:
    BasicTypeDeclaration() : m_access(Access_public) {}
    virtual ~BasicTypeDeclaration() {}
    virtual bool isValid() const { return !m_name.isEmpty(); }
    
    void setName(const QString& name) { m_name = name; }
    QString name() const { return m_name; }
    
    void setNameSpace(const QString& nspace) { m_nspace = nspace; }
    QString nameSpace() const { return m_nspace; }

    void setParent(Class* parent) { m_parent = parent; }
    Class* parent() const { return m_parent; }

    void setAccess(Access access) { m_access = access; }
    Access access() const { return m_access; }

    void setFileName(const QString& fileName) { m_file = fileName; }
    QString fileName() const { return m_file; }

    QString toString() const;

protected:
    BasicTypeDeclaration(const QString& name, const QString& nspace = QString(), Class* parent = 0)
        : m_name(name), m_nspace(nspace), m_parent(parent) {}

    QString m_name;
    QString m_nspace;
    Class* m_parent;
    QString m_file;
    Access m_access;
};

class GENERATOR_EXPORT Class : public BasicTypeDeclaration
{
public:
    enum Kind {
        Kind_Class,
        Kind_Struct,
        Kind_Union
    };
    
    struct BaseClassSpecifier {
        Class *baseClass;
        Access access;
        bool isVirtual;
    };
    
    Class(const QString& name = QString(), const QString nspace = QString(), Class* parent = 0, Kind kind = Kind_Class, bool isForward = true)
          : BasicTypeDeclaration(name, nspace, parent), m_kind(kind), m_forward(isForward), m_isNamespace(false), m_isTemplate(false) {}
    virtual ~Class() {}
    
    void setKind(Kind kind) { m_kind = kind; }
    Kind kind() const { return m_kind; }
    
    void setIsForwardDecl(bool forward) { m_forward = forward; }
    bool isForwardDecl() const { return m_forward; }
    
    void setIsNameSpace(bool isNamespace) { m_isNamespace = isNamespace; }
    bool isNameSpace() const { return m_isNamespace; }
    
    const QList<Method>& methods() const { return m_methods; }
    QList<Method>& methodsRef() { return m_methods; }
    void appendMethod(const Method& method, bool checkForConstArguments = false);
    
    const QList<Field>& fields() const { return m_fields; }
    QList<Field>& fieldsRef() { return m_fields; }
    void appendField(const Field& field) {  m_fields.append(field); }
    
    const QList<BaseClassSpecifier>& baseClasses() const { return m_bases; }
    void appendBaseClass(const BaseClassSpecifier& baseClass) { m_bases.append(baseClass); }
    
    const QList<BasicTypeDeclaration*>& children() const { return m_children; }
    void appendChild(BasicTypeDeclaration* child) { m_children.append(child); }
    
    bool isTemplate() const { return m_isTemplate; }
    void setIsTemplate(bool isTemplate) { m_isTemplate = isTemplate; }
    
private:
    Kind m_kind;
    bool m_forward;
    bool m_isNamespace;
    bool m_isTemplate;
    QList<Method> m_methods;
    QList<Field> m_fields;
    QList<BaseClassSpecifier> m_bases;
    QList<BasicTypeDeclaration*> m_children;
};

class GENERATOR_EXPORT Typedef : public BasicTypeDeclaration
{
public:
    Typedef(Type* type = 0, const QString& name = QString(), const QString nspace = QString(), Class* parent = 0)
            : BasicTypeDeclaration(name, nspace, parent), m_type(type) {}
    virtual ~Typedef() {}

    virtual bool isValid() const { return (!m_name.isEmpty() && m_type); }

    void setType(Type* type) { m_type = type; }
    Type* type() const { return m_type; }

    Type resolve() const;

private:
    Type* m_type;
};

class EnumMember;

class GENERATOR_EXPORT Enum : public BasicTypeDeclaration
{
public:
    Enum(bool isScoped = false, const QString& name = QString(), const QString nspace = QString(), Class* parent = 0)
         : BasicTypeDeclaration(name, nspace, parent)
         , m_isScopedEnum(isScoped)
    {}

    const QList<EnumMember>& members() const { return m_members; }
    QList<EnumMember>& membersRef() { return m_members; }
    void appendMember(const EnumMember& member) { m_members.append(member); }

    QString qualifiedName() const {
        QString ret = m_nspace;
        if (!ret.isEmpty())
            ret += "::";
        ret += m_name;
        return ret;
    }

    bool isScoped() const {
        return m_isScopedEnum;
    }

private:
    QList<EnumMember> m_members;
    bool m_isScopedEnum;
};

class GENERATOR_EXPORT Member
{
public:
    enum Flag {
        Virtual = 0x1,
        PureVirtual = 0x2,
        Static = 0x4,
        DynamicDispatch = 0x8,
        Explicit = 0x10,
    };
    Q_DECLARE_FLAGS(Flags, Flag)

    Member(BasicTypeDeclaration* typeDecl = 0, const QString& name = QString(), Type* type = 0, Access access = Access_public)
        : m_typeDecl(typeDecl), m_name(name), m_type(type), m_access(access) {}
    virtual ~Member() {}

    bool isValid() const { return (!m_name.isEmpty() && m_type && m_typeDecl); }

    void setDeclaringType(Class* klass) { m_typeDecl = klass; }
    BasicTypeDeclaration* declaringType() const { return m_typeDecl; }

    void setName(const QString& name) { m_name = name; }
    QString name() const { return m_name; }

    void setType(Type* type) { m_type = type; }
    Type* type() const { return m_type; }

    void setAccess(Access access) { m_access = access; }
    Access access() const { return m_access; }

    void setFlag(Flag flag) { m_flags |= flag; }
    void removeFlag(Flag flag) { m_flags &= ~flag; }
    Flags flags() const { return m_flags; }

    QString toString(bool withAccess = false, bool withClass = false) const;

protected:
    BasicTypeDeclaration* m_typeDecl;
    QString m_name;
    Type* m_type;
    Access m_access;
    Flags m_flags;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(Member::Flags)

class GENERATOR_EXPORT EnumMember : public Member
{
public:
    EnumMember(Enum* e = 0, const QString& name = QString(), const QString& value = QString(), Type* type = 0)
        : Member(e, name, type), m_value(value) {}

    Enum* getEnum() const { return static_cast<Enum*>(m_typeDecl); }

    void setValue(const QString& value) { m_value = value; }
    QString value() const { return m_value; }

    QString toString() const;

protected:
    QString m_value;
};

class GENERATOR_EXPORT Parameter
{
public:
    Parameter(const QString& name = QString(), Type* type = 0, const QString& defaultValue = QString())
        : m_name(name), m_type(type), m_defaultValue(defaultValue) {}
    virtual ~Parameter() {}

    bool isValid() const { return m_type; }

    void setName(const QString& name) { m_name = name; }
    QString name() const { return m_name; }

    void setType(Type* type) { m_type = type; }
    Type* type() const { return m_type; }

    bool isDefault() const { return !m_defaultValue.isEmpty(); }

    QString defaultValue() const { return m_defaultValue; }
    void setDefaultValue(const QString& value) { m_defaultValue = value; }

    QString toString() const;

protected:
    QString m_name;
    Type* m_type;
    QString m_defaultValue;
};

using ParameterList = QList<Parameter>;

class GENERATOR_EXPORT Method : public Member
{
public:
    Method(Class* klass = 0, const QString& name = QString(), Type* type = 0, Access access = Access_public, ParameterList params = ParameterList())
        : Member(klass, name, type, access), m_params(params), m_isConstructor(false), m_isDestructor(false), m_isConst(false), m_is_accessor(false),
          m_hasExceptionSpec(false), m_isSignal(false), m_isSlot(false) {}
    virtual ~Method() {}

    Class* getClass() const { return static_cast<Class*>(m_typeDecl); }

    const ParameterList& parameters() const { return m_params; }
    void appendParameter(const Parameter& param) { m_params.append(param); }
    void setParameterList(const ParameterList& params) { m_params = params; }

    void setIsConstructor(bool isCtor) { m_isConstructor = isCtor; }
    bool isConstructor() const { return m_isConstructor; }

    void setIsDestructor(bool isDtor) { m_isDestructor = isDtor; }
    bool isDestructor() const { return m_isDestructor; }

    void setIsConst(bool isConst) { m_isConst = isConst; }
    bool isConst() const { return m_isConst; }

    void setIsQPropertyAccessor(bool isAccessor) { m_is_accessor = isAccessor; }
    bool isQPropertyAccessor() const { return m_is_accessor; }

    void setIsSignal(bool isSignal) { m_isSignal = isSignal; }
    bool isSignal() const { return m_isSignal; }
    
    void setIsSlot(bool isSlot) { m_isSlot = isSlot; }
    bool isSlot() const { return m_isSlot; }

    void setIsDeleted(bool isDeleted) { m_isDeleted = isDeleted; }
    bool isDeleted() const { return m_isDeleted; }

    // TODO: This actually doesn't belong here. Better add a dynamic property system to Member subclasses.
    //       Then we can also get rid of the various method => foo maps in the 'Util' struct.
    const QStringList& remainingDefaultValues() const { return m_remainingValues; }
    void setRemainingDefaultValues(const QStringList& params) { m_remainingValues = params; }

    void setHasExceptionSpec(bool hasSpec) { m_hasExceptionSpec = hasSpec; }
    bool hasExceptionSpec() const { return m_hasExceptionSpec; }

    // Field accessor support - stores the Field this method is an accessor for
    void setAccessorField(const Field* field) { m_accessorField = field; }
    const Field* accessorField() const { return m_accessorField; }

    // Global function support - stores the Function this method wraps (for namespace functions)
    void setGlobalFunction(const Function* func) { m_globalFunction = func; }
    const Function* globalFunction() const { return m_globalFunction; }

    void appendExceptionType(const Type& type) { m_exceptionTypes.append(type); }
    const QList<Type>& exceptionTypes() const { return m_exceptionTypes; }

    QString toString(bool withAccess = false, bool withClass = false, bool withInitializer = true) const;

protected:
    ParameterList m_params;
    bool m_isConstructor;
    bool m_isDestructor;
    bool m_isConst;
    bool m_is_accessor;
    bool m_hasExceptionSpec;
    bool m_isSignal;
    bool m_isSlot;
    bool m_isDeleted;
    const Field* m_accessorField = nullptr;
    const Function* m_globalFunction = nullptr;
    QList<Type> m_exceptionTypes;
    QStringList m_remainingValues;
};

class GENERATOR_EXPORT Field : public Member
{
public:
    Field(Class* klass = 0, const QString& name = QString(), Type* type = 0, Access access = Access_public)
        : Member(klass, name, type, access) {}
    virtual ~Field() {}

    Class* getClass() const { return static_cast<Class*>(m_typeDecl); }
};

class GENERATOR_EXPORT GlobalVar
{
public:
    GlobalVar(const QString& name = QString(), const QString nspace = QString(), Type* type = 0) : m_name(name), m_nspace(nspace), m_type(type) {}
    virtual ~GlobalVar() {}

    bool isValid() const { return (!m_name.isEmpty() && m_type); }

    void setName(const QString& name) { m_name = name; }
    QString name() const { return m_name; }
    QString qualifiedName() const {
        QString ret = m_nspace;
        if (!ret.isEmpty())
            ret += "::";
        ret += m_name;
        return ret;
    }

    void setNameSpace(const QString& nspace) { m_nspace = nspace; }
    QString nameSpace() const { return m_nspace; }

    void setType(Type* type) { m_type = type; }
    Type* type() const { return m_type; }

    void setFileName(const QString& fileName) { m_file = fileName; }
    QString fileName() const { return m_file; }

    virtual QString toString(bool prepend) const;

protected:
    QString m_name;
    QString m_nspace;
    Type* m_type;
    QString m_file;
};

class GENERATOR_EXPORT Function : public GlobalVar
{
public:
    Function(const QString& name = QString(), const QString nspace = QString(), Type* type = 0, ParameterList params = ParameterList())
        : GlobalVar(name, nspace, type), m_params(params) {}

    const ParameterList& parameters() const { return m_params; }
    void appendParameter(const Parameter& param) { m_params.append(param); }

    virtual QString toString(bool prepend) const;

protected:
    ParameterList m_params;
};

class GENERATOR_EXPORT Type
{
public:
    Type(Class* klass = 0, bool isConst = false, bool isVolatile = false, int pointerDepth = 0, bool isRef = false)
        : m_class(klass), m_typedef(0), m_enum(0), m_isConst(isConst), m_isVolatile(isVolatile), 
          m_pointerDepth(pointerDepth), m_isRef(isRef), m_isIntegral(false), m_isFunctionPointer(false) {}
    Type(Typedef* tdef, bool isConst = false, bool isVolatile = false, int pointerDepth = 0, bool isRef = false)
        : m_class(0), m_typedef(tdef), m_enum(0), m_isConst(isConst), m_isVolatile(isVolatile), 
          m_pointerDepth(pointerDepth), m_isRef(isRef), m_isIntegral(false), m_isFunctionPointer(false) {}
    Type(Enum* e, bool isConst = false, bool isVolatile = false, int pointerDepth = 0, bool isRef = false)
        : m_class(0), m_typedef(0), m_enum(e), m_isConst(isConst), m_isVolatile(isVolatile), 
          m_pointerDepth(pointerDepth), m_isRef(isRef), m_isIntegral(false), m_isFunctionPointer(false) {}
    Type(const QString& name, bool isConst = false, bool isVolatile = false, int pointerDepth = 0, bool isRef = false)
        : m_class(0), m_typedef(0), m_enum(0), m_name(name), m_isConst(isConst), m_isVolatile(isVolatile), 
          m_pointerDepth(pointerDepth), m_isRef(isRef), m_isIntegral(false), m_isFunctionPointer(false) {}

    void setClass(Class* klass) { m_class = klass; m_typedef = 0; m_enum = 0; }
    Class* getClass() const { return m_class; }
    
    void setTypedef(Typedef* tdef) { m_typedef = tdef; m_class = 0; m_enum = 0; }
    Typedef* getTypedef() const { return m_typedef; }

    void setEnum(Enum* e) { m_enum = e; m_class = 0; m_typedef = 0; }
    Enum* getEnum() const { return m_enum; }

    void setName(const QString& name) { m_name = name; }
    QString name(bool prepend = true) const {
        if (m_class) {
            return (prepend ? "::": "") + m_class->toString();
        } else if (m_typedef) {
            return m_typedef->toString();
        } else if (m_enum) {
            return m_enum->toString();
        } else {
            return m_name;
        }
    }

    bool isValid() const { return (m_class || m_typedef || !m_name.isEmpty()); }
    
    void setIsConst(bool isConst) { m_isConst = isConst; }
    bool isConst() const { return m_isConst; }
    void setIsVolatile(bool isVolatile) { m_isVolatile = isVolatile; }
    bool isVolatile() const { return m_isVolatile; }
    
    void setPointerDepth(int depth) { m_pointerDepth = depth; }
    int pointerDepth() const { return m_pointerDepth; }
    
    void setIsConstPointer(int depth, bool isConst) { m_constPointer[depth] = isConst; }
    bool isConstPointer(int depth) const { return m_constPointer.value(depth, false); }
    
    void setIsRef(bool isRef) { m_isRef = isRef; }
    bool isRef() const { return m_isRef; }

    void setIsIntegral(bool isIntegral) { m_isIntegral = isIntegral; }
    bool isIntegral() const { return m_isIntegral; }

    void setArrayDimensions(int dim) { m_arrayLengths.resize(dim); }
    int arrayDimensions() const { return m_arrayLengths.size(); }
    bool isArray() const { return m_arrayLengths.size(); }

    void setArrayLength(int dim, int length) { m_arrayLengths[dim] = length; }
    int arrayLength(int dim) const { return m_arrayLengths[dim]; }

    const QList<Type>& templateArguments() const { return m_templateArgs; }
    void appendTemplateArgument(const Type& type) { m_templateArgs.append(type); }
    void setTemplateArguments(const QList<Type>& types) { m_templateArgs = types; }

    void setIsFunctionPointer(bool isPtr) { m_isFunctionPointer = isPtr; }
    bool isFunctionPointer() const { return m_isFunctionPointer; }
    const ParameterList& parameters() const { return m_params; }
    void appendParameter(const Parameter& param) { m_params.append(param); }

    QString toString(const QString& fnPtrName = QString(), bool prepend = true) const;

    bool isAssignable();

    // on windows, we can't reference 'types' here, because it's marked __declspec(dllexport) above.
    static Type* registerType(const Type& type)
#ifndef Q_OS_WIN
    {
        QString typeString = type.toString();
        auto res = types.insert(typeString, type);
        return &res.value();
    }
#else
    ;
#endif

    static const Type* Void;

protected:
    Class* m_class;
    Typedef* m_typedef;
    Enum* m_enum;
    QString m_name;
    bool m_isConst, m_isVolatile;
    int m_pointerDepth;
    QHash<int, bool> m_constPointer;
    bool m_isRef;
    bool m_isIntegral;
    QList<Type> m_templateArgs;
    bool m_isFunctionPointer;
    ParameterList m_params;
    QVector<int> m_arrayLengths;
};

#endif // TYPE_H
