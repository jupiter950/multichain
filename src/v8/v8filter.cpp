// Copyright (c) 2014-2018 Coin Sciences Ltd
// MultiChain code distributed under the GPLv3 license, see COPYING file.

#include "v8filter.h"
#include "callbacks.h"
#include "chainparams/state.h"
#include "utils/define.h"
#include "utils/tinyformat.h"
#include "utils/util.h"
#include "v8engine.h"
#include "v8utils.h"
#include <cassert>

namespace mc_v8
{
static std::string jsFixture = R"(
Math.random = function() {
    return 0;
};

Date.now = function() {
    return 0;
};

var bind = Function.bind;
var unbind = bind.bind(bind);

function instantiate(constructor, args) {
    return new (unbind(constructor, null).apply(null, args));
}

Date = function (Date) {
    var names = Object.getOwnPropertyNames(Date);
    // Loop through them
    for (var i = 0; i < names.length; i++) {
        // Skip props already in the MyDate object
        if (names[i] in MyDate) continue;
        // Get property description from o
        var desc = Object.getOwnPropertyDescriptor(Date, names[i]);
        // Use it to create property on MyDate
        Object.defineProperty(MyDate, names[i], desc);
    }

    return MyDate;

    function MyDate() {
        if (arguments.length == 0) {
            arguments = [0];
        }
        return instantiate(Date, arguments);
    }
}(Date);
)";

static std::string jsLimitMathSet = R"(
var mathKeep = new Set(["abs", "ceil", "floor", "max", "min", "round", "sign", "trunc", "log", "log10", "log2", "pow",
    "sqrt", "E", "LN10", "LN2", "LOG10E", "LOG2E", "PI", "SQRT1_2", "SQRT2" ]);
for (var fn of Object.getOwnPropertyNames(Math)) {
    if (! mathKeep.has(fn)) {
        delete Math[fn];
    }
}
delete Date.now;
)";

V8Filter::V8Filter()
{
    Zero();
}

V8Filter::~V8Filter()
{
    Destroy();
}

void V8Filter::Zero()
{
    m_engine = nullptr;
    m_isRunning = false;
}

int V8Filter::Destroy()
{
    if (m_isRunning)
    {
        m_engine->GetIsolate()->TerminateExecution();
    }
    m_filterFunction.Reset();
    m_context.Reset();
    this->Zero();
    return MC_ERR_NOERROR;
}

int V8Filter::Initialize(V8Engine *engine, std::string script, std::string functionName,
                         std::vector<std::string> &callback_names, std::string &strResult)
{
    if (fDebug)
        LogPrint("v8filter", "v8filter: V8Filter::Initialize\n");
    m_engine = engine;
    v8::Isolate *isolate = m_engine->GetIsolate();
    strResult.clear();
    v8::Locker locker(isolate);
    v8::Isolate::Scope isolateScope(isolate);
    v8::HandleScope handleScope(isolate);
    auto global = v8::ObjectTemplate::New(isolate);
    auto isolateData = v8::External::New(isolate, m_engine->GetIsolateData());
    for (std::string functionName : callback_names)
    {
        if (callbackLookup.find(functionName) == callbackLookup.end())
        {
            strResult = strprintf("Undefined callback name: {}", functionName);
            return MC_ERR_INTERNAL_ERROR;
        }
        global->Set(String2V8(isolate, functionName),
                    v8::FunctionTemplate::New(isolate, callbackLookup[functionName], isolateData));
    }
    auto context = v8::Context::New(isolate, nullptr, global);
    m_context.Reset(isolate, context);

    std::string jsPreamble = jsFixture;
    if (mc_gState->m_Features->FilterLimitedMathSet())
    {
        jsPreamble += jsLimitMathSet;
    }

    int status = this->CompileAndLoadScript(jsPreamble, "", "preamble", strResult);
    if (status != MC_ERR_NOERROR || !strResult.empty())
    {
        m_context.Reset();
        return status;
    }
    status = this->CompileAndLoadScript(script, functionName, "<script>", strResult);
    if (status != MC_ERR_NOERROR)
    {
        m_context.Reset();
    }
    return status;
}

int V8Filter::Run(std::string &strResult, bool withCallbackLog)
{
    if (fDebug)
        LogPrint("v8filter", "v8filter: V8Filter::Run\n");

    v8::Isolate *isolate = m_engine->GetIsolate();
    strResult.clear();
    if (m_context.IsEmpty() || m_filterFunction.IsEmpty())
    {
        strResult = "Trying to run an invalid filter";
        return MC_ERR_NOERROR;
    }
    v8::Locker locker(isolate);
    v8::Isolate::Scope isolateScope(isolate);
    v8::HandleScope handleScope(isolate);
    v8::TryCatch tryCatch(isolate);
    auto context = v8::Local<v8::Context>::New(isolate, m_context);
    v8::Context::Scope contextScope(context);
    m_engine->GetIsolateData()->Reset(withCallbackLog);

    v8::Local<v8::Value> result;
    auto filterFunction = v8::Local<v8::Function>::New(isolate, m_filterFunction);
    m_isRunning = true;
    bool ok = filterFunction->Call(context, context->Global(), 0, nullptr).ToLocal(&result);
    m_isRunning = false;
    if (!ok)
    {
        assert(tryCatch.HasCaught());
        if (tryCatch.Exception()->IsNull() && tryCatch.Message().IsEmpty())
        {
            strResult = m_engine->TerminationReason();
        }
        else
        {
            this->ReportException(&tryCatch, strResult);
        }
        return MC_ERR_NOERROR;
    }

    if (result->IsString())
    {
        strResult = V82String(isolate, result);
    }

    return MC_ERR_NOERROR;
}

int V8Filter::RunWithCallbackLog(std::string &strResult, json_spirit::Array &callbacks)
{
    int retcode = this->Run(strResult, true);
    callbacks = m_engine->GetIsolateData()->callbacks;
    return retcode;
}

int V8Filter::CompileAndLoadScript(std::string script, std::string functionName, std::string source,
                                   std::string &strResult)
{
    if (fDebug)
        LogPrint("v8filter", "v8filter: V8Filter::CompileAndLoadScript %s\n", source);

    v8::Isolate *isolate = m_engine->GetIsolate();
    strResult.clear();
    v8::HandleScope handleScope(isolate);
    v8::TryCatch tryCatch(isolate);
    auto context = v8::Local<v8::Context>::New(isolate, m_context);
    v8::Context::Scope contextScope(context);
    v8::ScriptOrigin scriptOrigin(String2V8(isolate, source));
    v8::Local<v8::String> v8script = String2V8(isolate, script);

    v8::Local<v8::Script> compiledScript;
    if (!v8::Script::Compile(context, v8script, &scriptOrigin).ToLocal(&compiledScript))
    {
        assert(tryCatch.HasCaught());
        this->ReportException(&tryCatch, strResult);
        return MC_ERR_NOERROR;
    }

    v8::Local<v8::Value> result;
    if (!compiledScript->Run(context).ToLocal(&result))
    {
        assert(tryCatch.HasCaught());
        this->ReportException(&tryCatch, strResult);
        return MC_ERR_NOERROR;
    }

    if (!functionName.empty())
    {
        v8::Local<v8::String> processName = String2V8(isolate, functionName);
        v8::Local<v8::Value> processVal;
        if (!context->Global()->Get(context, processName).ToLocal(&processVal) || !processVal->IsFunction())
        {
            strResult = tfm::format("Cannot find function '%s' in script", functionName);
            return MC_ERR_NOERROR;
        }
        m_filterFunction.Reset(isolate, v8::Local<v8::Function>::Cast(processVal));
    }
    return MC_ERR_NOERROR;
}

void V8Filter::ReportException(v8::TryCatch *tryCatch, std::string &strResult)
{
    if (fDebug)
        LogPrint("v8filter", "v8filter: V8Filter: ReportException\n");

    v8::Isolate *isolate = m_engine->GetIsolate();
    v8::HandleScope handlecope(isolate);
    strResult = V82String(isolate, tryCatch->Exception());
    v8::Local<v8::Message> message = tryCatch->Message();
    if (message.IsEmpty())
    {
        if (fDebug)
            LogPrint("v8filter", "v8filter: %s", strResult);
    }
    else
    {
        std::string filename = V82String(isolate, message->GetScriptResourceName());
        auto context = v8::Local<v8::Context>::New(isolate, m_context);
        v8::Context::Scope contextScope(context);
        int linenum = message->GetLineNumber(context).FromJust();
        int start = message->GetStartColumn(context).FromJust();
        int end = message->GetEndColumn(context).FromJust();
        assert(linenum >= 0 && start >= 0 && end >= 0);
        if (fDebug)
            LogPrint("v8filter", "v8filter: %s:%d %s\n", filename, linenum, strResult);
        std::string sourceline = V82String(isolate, message->GetSourceLine(context).ToLocalChecked());
        if (fDebug)
            LogPrint("v8filter", "v8filter: %s\n", sourceline);
        if (fDebug)
            LogPrint("v8filter", "v8filter: %s%s\n", std::string(static_cast<std::string::size_type>(start), ' '),
                     std::string(static_cast<std::string::size_type>(end - start), '^'));
    }
}

} // namespace mc_v8
