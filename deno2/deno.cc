/*
Copyright 2018 Ryan Dahl <ry@tinyclouds.org>. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to
deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.
*/
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "v8/include/libplatform/libplatform.h"
#include "v8/include/v8.h"

#include "./deno_internal.h"
#include "include/deno.h"

#define CHECK(x) assert(x)  // TODO(ry) use V8's CHECK.

namespace deno {

// Extracts a C string from a v8::V8 Utf8Value.
const char* ToCString(const v8::String::Utf8Value& value) {
  return *value ? *value : "<string conversion failed>";
}

static inline v8::Local<v8::String> v8_str(const char* x) {
  return v8::String::NewFromUtf8(v8::Isolate::GetCurrent(), x,
                                 v8::NewStringType::kNormal)
      .ToLocalChecked();
}

void HandleException(v8::Local<v8::Context> context,
                     v8::Local<v8::Value> exception) {
  auto* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);
  v8::Context::Scope context_scope(context);

  auto message = v8::Exception::CreateMessage(isolate, exception);
  auto onerrorStr = v8::String::NewFromUtf8(isolate, "onerror");
  auto onerror = context->Global()->Get(onerrorStr);

  if (onerror->IsFunction()) {
    auto func = v8::Local<v8::Function>::Cast(onerror);
    v8::Local<v8::Value> args[5];
    auto origin = message->GetScriptOrigin();
    args[0] = exception->ToString();
    args[1] = message->GetScriptResourceName();
    args[2] = origin.ResourceLineOffset();
    args[3] = origin.ResourceColumnOffset();
    args[4] = exception;
    func->Call(context->Global(), 5, args);
    /* message, source, lineno, colno, error */
  } else {
    v8::String::Utf8Value exceptionStr(isolate, exception);
    printf("Unhandled Exception %s\n", ToCString(exceptionStr));
    message->PrintCurrentStackTrace(isolate, stdout);
  }
}

/*
bool AbortOnUncaughtExceptionCallback(v8::Isolate* isolate) {
  return true;
}

void MessageCallback2(Local<Message> message, v8::Local<v8::Value> data) {
  printf("MessageCallback2\n\n");
}

void FatalErrorCallback2(const char* location, const char* message) {
  printf("FatalErrorCallback2\n");
}
*/

void ExitOnPromiseRejectCallback(
    v8::PromiseRejectMessage promise_reject_message) {
  auto* isolate = v8::Isolate::GetCurrent();
  Deno* d = static_cast<Deno*>(isolate->GetData(0));
  assert(d->isolate == isolate);
  v8::HandleScope handle_scope(d->isolate);
  auto exception = promise_reject_message.GetValue();
  auto context = d->context.Get(d->isolate);
  HandleException(context, exception);
}

void Print(const v8::FunctionCallbackInfo<v8::Value>& args) {
  assert(args.Length() == 1);
  auto* isolate = args.GetIsolate();
  v8::HandleScope handle_scope(isolate);
  v8::String::Utf8Value str(isolate, args[0]);
  const char* cstr = ToCString(str);
  printf("%s\n", cstr);
  fflush(stdout);
}

// Sets the sub callback.
void Sub(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  Deno* d = reinterpret_cast<Deno*>(isolate->GetData(0));
  assert(d->isolate == isolate);

  v8::HandleScope handle_scope(isolate);

  if (!d->sub.IsEmpty()) {
    isolate->ThrowException(v8_str("denoSub already called."));
    return;
  }

  v8::Local<v8::Value> v = args[0];
  assert(v->IsFunction());
  v8::Local<v8::Function> func = v8::Local<v8::Function>::Cast(v);

  d->sub.Reset(isolate, func);
}

void Pub(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  Deno* d = static_cast<Deno*>(isolate->GetData(0));
  assert(d->isolate == isolate);

  v8::Locker locker(d->isolate);
  v8::EscapableHandleScope handle_scope(isolate);

  assert(args.Length() == 2);
  v8::Local<v8::Value> channel_v = args[0];
  assert(channel_v->IsString());
  v8::String::Utf8Value channel_vstr(isolate, channel_v);
  const char* channel = *channel_vstr;

  v8::Local<v8::Value> ab_v = args[1];
  assert(ab_v->IsArrayBuffer());

  auto ab = v8::Local<v8::ArrayBuffer>::Cast(ab_v);
  auto contents = ab->GetContents();

  // data is only a valid pointer until the end of this call.
  const char* data =
      const_cast<const char*>(reinterpret_cast<char*>(contents.Data()));
  deno_buf buf{data, contents.ByteLength()};

  auto retbuf = d->cb(d, channel, buf);
  if (retbuf.data) {
    // TODO(ry) Support zero-copy.
    auto ab = v8::ArrayBuffer::New(d->isolate, retbuf.len);
    memcpy(ab->GetContents().Data(), retbuf.data, retbuf.len);
    args.GetReturnValue().Set(handle_scope.Escape(ab));
  } else {
    args.GetReturnValue().Set(v8::Null(d->isolate));
  }
}

bool Execute(v8::Local<v8::Context> context, const char* js_filename,
             const char* js_source) {
  auto* isolate = context->GetIsolate();
  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope handle_scope(isolate);

  v8::Context::Scope context_scope(context);

  v8::TryCatch try_catch(isolate);

  auto name = v8_str(js_filename);
  auto source = v8_str(js_source);

  v8::ScriptOrigin origin(name);

  auto script = v8::Script::Compile(context, source, &origin);

  if (script.IsEmpty()) {
    assert(try_catch.HasCaught());
    HandleException(context, try_catch.Exception());
    return false;
  }

  auto result = script.ToLocalChecked()->Run(context);

  if (result.IsEmpty()) {
    assert(try_catch.HasCaught());
    HandleException(context, try_catch.Exception());
    return false;
  }

  return true;
}

v8::StartupData SerializeInternalFields(v8::Local<v8::Object> holder, int index,
                                        void* data) {
  assert(data == nullptr);  // TODO(ry) pass Deno* object here.
  InternalFieldData* embedder_field = static_cast<InternalFieldData*>(
      holder->GetAlignedPointerFromInternalField(index));
  if (embedder_field == nullptr) return {nullptr, 0};
  int size = sizeof(*embedder_field);
  char* payload = new char[size];
  // We simply use memcpy to serialize the content.
  memcpy(payload, embedder_field, size);
  return {payload, size};
}

v8::StartupData MakeSnapshot(v8::StartupData* prev_natives_blob,
                             v8::StartupData* prev_snapshot_blob,
                             const char* js_filename, const char* js_source) {
  v8::V8::SetNativesDataBlob(prev_natives_blob);
  v8::V8::SetSnapshotDataBlob(prev_snapshot_blob);

  auto* creator = new v8::SnapshotCreator(external_references);
  auto* isolate = creator->GetIsolate();
  v8::Isolate::Scope isolate_scope(isolate);
  {
    v8::HandleScope handle_scope(isolate);
    auto context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);

    auto global = context->Global();
    // TODO(ry) Add a global namespace object "deno" and move print, sub, and
    // pub inside that object.
    auto print_tmpl = v8::FunctionTemplate::New(isolate, Print);
    auto print_val = print_tmpl->GetFunction(context).ToLocalChecked();
    CHECK(
        global->Set(context, deno::v8_str("denoPrint"), print_val).FromJust());

    auto sub_tmpl = v8::FunctionTemplate::New(isolate, Sub);
    auto sub_val = sub_tmpl->GetFunction(context).ToLocalChecked();
    CHECK(global->Set(context, deno::v8_str("denoSub"), sub_val).FromJust());

    auto pub_tmpl = v8::FunctionTemplate::New(isolate, Pub);
    auto pub_val = pub_tmpl->GetFunction(context).ToLocalChecked();
    CHECK(global->Set(context, deno::v8_str("denoPub"), pub_val).FromJust());

    bool r = Execute(context, js_filename, js_source);
    CHECK(r);

    creator->SetDefaultContext(context, v8::SerializeInternalFieldsCallback(
                                            SerializeInternalFields, nullptr));
  }

  auto snapshot_blob =
      creator->CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kKeep);

  return snapshot_blob;
}

void AddIsolate(Deno* d, v8::Isolate* isolate) {
  d->isolate = isolate;
  // Leaving this code here because it will probably be useful later on, but
  // disabling it now as I haven't got tests for the desired behavior.
  // d->isolate->SetCaptureStackTraceForUncaughtExceptions(true);
  // d->isolate->SetAbortOnUncaughtExceptionCallback(AbortOnUncaughtExceptionCallback);
  // d->isolate->AddMessageListener(MessageCallback2);
  // d->isolate->SetFatalErrorHandler(FatalErrorCallback2);
  d->isolate->SetPromiseRejectCallback(deno::ExitOnPromiseRejectCallback);
  d->isolate->SetData(0, d);
}

}  // namespace deno

extern "C" {

void deno_init() {
  // v8::V8::InitializeICUDefaultLocation(argv[0]);
  // v8::V8::InitializeExternalStartupData(argv[0]);
  auto* p = v8::platform::CreateDefaultPlatform();
  v8::V8::InitializePlatform(p);
  v8::V8::Initialize();
}

const char* deno_v8_version() { return v8::V8::GetVersion(); }

void deno_set_flags(int* argc, char** argv) {
  v8::V8::SetFlagsFromCommandLine(argc, argv, true);
}

const char* deno_last_exception(Deno* d) { return d->last_exception.c_str(); }

bool deno_execute(Deno* d, const char* js_filename, const char* js_source) {
  auto* isolate = d->isolate;
  v8::Locker locker(isolate);
  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope handle_scope(isolate);
  auto context = d->context.Get(d->isolate);
  return deno::Execute(context, js_filename, js_source);
}

bool deno_pub(Deno* d, const char* channel, deno_buf buf) {
  v8::Locker locker(d->isolate);
  v8::Isolate::Scope isolate_scope(d->isolate);
  v8::HandleScope handle_scope(d->isolate);

  auto context = d->context.Get(d->isolate);
  v8::Context::Scope context_scope(context);

  v8::TryCatch try_catch(d->isolate);

  auto sub = d->sub.Get(d->isolate);
  if (sub.IsEmpty()) {
    d->last_exception = "deno_sub has not been called.";
    return false;
  }

  // TODO(ry) support zero-copy.
  auto ab = v8::ArrayBuffer::New(d->isolate, buf.len);
  memcpy(ab->GetContents().Data(), buf.data, buf.len);

  v8::Local<v8::Value> args[2];
  args[0] = deno::v8_str(channel);
  args[1] = ab;

  sub->Call(context->Global(), 1, args);

  if (try_catch.HasCaught()) {
    deno::HandleException(context, try_catch.Exception());
    return false;
  }

  return true;
}

void deno_delete(Deno* d) {
  d->isolate->Dispose();
  delete d;
}

void deno_terminate_execution(Deno* d) { d->isolate->TerminateExecution(); }

}  // extern "C"
