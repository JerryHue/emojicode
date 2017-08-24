//
//  PackageReporter.cpp
//  Emojicode
//
//  Created by Theo Weidmann on 02/05/16.
//  Copyright © 2016 Theo Weidmann. All rights reserved.
//

#include "PackageReporter.hpp"
#include "EmojicodeCompiler.hpp"
#include "Function.hpp"
#include "Initializer.hpp"
#include "Types/Class.hpp"
#include "Types/Enum.hpp"
#include "Types/Protocol.hpp"
#include "Types/TypeContext.hpp"
#include "Types/TypeDefinition.hpp"
#include "Types/ValueType.hpp"
#include <cstring>
#include <list>
#include <map>
#include <vector>

namespace EmojicodeCompiler {

enum class ReturnKind {
    Return,
    NoReturn,
    ErrorProneInitializer
};

void reportDocumentation(const std::u32string &documentation) {
    if (documentation.empty()) {
        return;
    }

    printf("\"documentation\":");
    printJSONStringToFile(utf8(documentation).c_str(), stdout);
    putc(',', stdout);
}

void reportType(const Type &type, const TypeContext &tc) {
    auto returnTypeName = type.toString(tc);
    printf("{\"package\":\"%s\",\"name\":\"%s\",\"optional\":%s}",
           type.typePackage().c_str(), returnTypeName.c_str(), type.optional() ? "true" : "false");
}

class CommaPrinter {
public:
    CommaPrinter() = default;
    void print() {
        if (printedFirst_) {
            putc(',', stdout);
        }
        printedFirst_ = true;
    }
private:
    bool printedFirst_ = false;
};

void reportGenericArguments(std::map<std::u32string, Type> map, std::vector<Type> constraints,
                            size_t superCount, const TypeContext &tc) {
    printf("\"genericArguments\":[");

    auto gans = std::vector<std::u32string>(map.size());
    for (auto it : map) {
        gans[it.second.genericVariableIndex() - superCount] = it.first;
    }

    CommaPrinter printer;
    for (size_t i = 0; i < gans.size(); i++) {
        printer.print();
        auto gan = gans[i];
        printf("{\"name\":");
        printJSONStringToFile(utf8(gan).c_str(), stdout);
        printf(",\"constraint\":");
        reportType(constraints[i], tc);
        printf("}");
    }

    printf("],");
}

void reportFunction(Function *function, ReturnKind returnKind, const TypeContext &tc) {
    printf("{\"name\":\"%s\",", utf8(function->name()).c_str());
    switch (function->accessLevel()) {
        case AccessLevel::Private:
            printf("\"access\":\"🔒\",");
            break;
        case AccessLevel::Protected:
            printf("\"access\":\"🔐\",");
            break;
        case AccessLevel::Public:
            printf("\"access\":\"🔓\",");
            break;
    }

    if (returnKind == ReturnKind::Return) {
        printf("\"returnType\":");
        reportType(function->returnType, tc);
        putc(',', stdout);
    }
    else if (returnKind == ReturnKind::ErrorProneInitializer) {
        printf("\"errorType\":");
        reportType(dynamic_cast<Initializer *>(function)->errorType(), tc);
        putc(',', stdout);
    }

    reportGenericArguments(function->genericArgumentVariables, function->genericArgumentConstraints, 0, tc);
    reportDocumentation(function->documentation());

    printf("\"arguments\":[");
    CommaPrinter printer;
    for (auto &argument : function->arguments) {
        printer.print();
        printf("{\"type\":");
        reportType(argument.type, tc);
        printf(",\"name\":");
        printJSONStringToFile(utf8(argument.variableName).c_str(), stdout);
        printf("}");
    }
    printf("]}");
}

template <typename T>
class TypeDefinitionReporter {
public:
    explicit TypeDefinitionReporter(T *typeDef) : typeDef_(typeDef) {}

    void report() const {
        reportBasics();
        printf("}");
    }
protected:
    T *typeDef_;

    virtual void reportBasics() const {
        printf("{\"name\": \"%s\",", utf8(typeDef_->name()).c_str());

        printf("\"conformsTo\":[");
        CommaPrinter printer;
        for (auto &protocol : typeDef_->protocols()) {
            printer.print();
            reportType(protocol, TypeContext(Type(typeDef_, false)));
        }
        printf("],");

        reportGenericArguments(typeDef_->ownGenericArgumentVariables(), typeDef_->genericArgumentConstraints(),
                               typeDef_->superGenericArguments().size(), TypeContext(Type(typeDef_, false)));
        reportDocumentation(typeDef_->documentation());

        printf("\"methods\":[");
        printFunctions(typeDef_->methodList());
        printf("],");

        printf("\"initializers\":[");
        CommaPrinter initializerPrinter;
        for (auto initializer : typeDef_->initializerList()) {
            initializerPrinter.print();
            reportFunction(initializer, initializer->errorProne() ? ReturnKind::ErrorProneInitializer
                           : ReturnKind::NoReturn, TypeContext(Type(typeDef_, false), initializer));
        }
        printf("],");

        printf("\"typeMethods\":[");
        printFunctions(typeDef_->typeMethodList());
        printf("]");
    }

    void printFunctions(const std::vector<Function *> &functions) const {
        CommaPrinter printer;
        for (auto function : functions) {
            printer.print();
            reportFunction(function, ReturnKind::Return, TypeContext(Type(typeDef_, false), function));
        }
    }
};

class ClassReporter : public TypeDefinitionReporter<Class> {
public:
    explicit ClassReporter(Class *klass) : TypeDefinitionReporter(klass) {}

    void reportBasics() const override {
        TypeDefinitionReporter::reportBasics();
        if (typeDef_->superclass() != nullptr) {
            printf(",\"superclass\":{\"package\":\"%s\",\"name\":\"%s\"}",
                   typeDef_->superclass()->package()->name().c_str(), utf8(typeDef_->superclass()->name()).c_str());
        }
    }
};

class EnumReporter : public TypeDefinitionReporter<Enum> {
public:
    explicit EnumReporter(Enum *enumeration) : TypeDefinitionReporter(enumeration) {}

    void reportBasics() const override {
        TypeDefinitionReporter::reportBasics();
        printf(",\"values\":[");
        CommaPrinter printer;
        for (auto it : typeDef_->values()) {
            printer.print();
            printf("{");
            reportDocumentation(it.second.second);
            printf("\"value\":\"%s\"}", utf8(it.first).c_str());
        }
        printf("]");
    }
};

void reportPackage(Package *package) {
    std::list<Enum *> enums;
    std::list<Class *> classes;
    std::list<Protocol *> protocols;
    std::list<ValueType *> valueTypes;

    for (auto exported : package->exportedTypes()) {
        switch (exported.type.type()) {
            case TypeType::Class:
                classes.push_back(exported.type.eclass());
                break;
            case TypeType::Enum:
                enums.push_back(exported.type.eenum());
                break;
            case TypeType::Protocol:
                protocols.push_back(exported.type.protocol());
                break;
            case TypeType::ValueType:
                valueTypes.push_back(exported.type.valueType());
                break;
            default:
                break;
        }
    }

    printf("{");
    reportDocumentation(package->documentation());

    printf("\"valueTypes\":[");
    CommaPrinter valueTypePrinter;
    for (auto valueType : valueTypes) {
        valueTypePrinter.print();
        TypeDefinitionReporter<ValueType>(valueType).report();
    }
    printf("],");

    CommaPrinter classPrinter;
    printf("\"classes\":[");
    for (auto klass : classes) {
        classPrinter.print();
        ClassReporter(klass).report();
    }
    printf("],");

    printf("\"enums\": [");
    CommaPrinter enumPrinter;
    for (auto enumeration : enums) {
        enumPrinter.print();
        EnumReporter(enumeration).report();
    }
    printf("],");

    printf("\"protocols\":[");
    CommaPrinter protocolPrinter;
    for (auto protocol : protocols) {
        protocolPrinter.print();
        TypeDefinitionReporter<Protocol>(protocol).report();
    }
    printf("]}");
}

}  // namespace EmojicodeCompiler
