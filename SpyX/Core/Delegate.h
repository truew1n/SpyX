#ifndef TAPI_DELEGATE_H
#define TAPI_DELEGATE_H

#include <cassert>
#include <cstring>

struct SDefaultDelegatePolicy {};

template <typename TSignature, typename TUserPolicy = SDefaultDelegatePolicy>
class TDelegate;

template <typename TReturnType, typename... TParameterTypes, typename TUserPolicy>
class TDelegate<TReturnType(TParameterTypes...), TUserPolicy>
{
private:
    void *MObject = nullptr;
    using TStubType = TReturnType(*)(void *Object, void *Method, TParameterTypes...);
    TStubType MStub = nullptr;
    void *MMethod = nullptr;

public:
    TDelegate() = default;

    bool operator==(const TDelegate &Other) const {
        return MMethod == Other.MMethod;
    }

    void BindStatic(TReturnType(*Function)(TParameterTypes...))
    {
        MObject = reinterpret_cast<void *>(Function);
        MMethod = nullptr;
        MStub = &InvokeStatic;
    }

    template <typename TObject>
    void BindRaw(TObject *Object, TReturnType(TObject:: *Method)(TParameterTypes...))
    {
        MObject = Object;
        MMethod = *reinterpret_cast<void **>(&Method);
        MStub = &InvokeMember<TObject>;
    }

    template <typename TObject>
    void BindRaw(TObject *Object, TReturnType(TObject:: *Method)(TParameterTypes...) const)
    {
        MObject = Object;
        MMethod = *reinterpret_cast<void **>(&Method);
        MStub = &InvokeConstMember<TObject>;
    }

    void Unbind()
    {
        MObject = nullptr;
        MMethod = nullptr;
        MStub = nullptr;
    }

    bool IsBound() const
    {
        return MStub != nullptr;
    }

    TReturnType Execute(TParameterTypes... Arguments) const
    {
        assert(IsBound());
        return MStub(MObject, MMethod, Arguments...);
    }

private:
    static TReturnType InvokeStatic(void *Function, void *, TParameterTypes... Arguments)
    {
        auto Func = reinterpret_cast<TReturnType(*)(TParameterTypes...)>(Function);
        return Func(Arguments...);
    }

    template <typename TObject>
    static TReturnType InvokeMember(void *Object, void *Method, TParameterTypes... Arguments)
    {
        TObject *ObjectInstance = static_cast<TObject *>(Object);
        TReturnType(TObject:: * Func)(TParameterTypes...);
        std::memcpy(&Func, &Method, sizeof(Func));
        return (ObjectInstance->*Func)(Arguments...);
    }

    template <typename TObject>
    static TReturnType InvokeConstMember(void *Object, void *Method, TParameterTypes... Arguments)
    {
        TObject *ObjectInstance = static_cast<TObject *>(Object);
        TReturnType(TObject:: * Func)(TParameterTypes...) const;
        std::memcpy(&Func, &Method, sizeof(Func));
        return (ObjectInstance->*Func)(Arguments...);
    }
};

#endif
