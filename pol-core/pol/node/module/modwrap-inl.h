#ifndef _H_MODWRAP_INL
#define _H_MODWRAP_INL

#include "../../polsem.h"
#include "modwrap.h"
#include "objwrap.h"

using namespace Pol;
using namespace Napi;

namespace Pol
{
namespace Node
{
template <typename PolModule>
Napi::FunctionReference NodeModuleWrap<PolModule>::constructor;

template <typename PolModule>
Napi::Value NodeModuleWrap<PolModule>::MethodWrapper( const CallbackInfo& cbinfo )
{
  Napi::Env env = cbinfo.Env();
  auto funcIdx = *( static_cast<int*>( cbinfo.Data() ) );
  NODELOG << "funcidx is " << funcIdx << "\n";
  auto& funcDef = PolModule::function_table.at( funcIdx );
  auto jsArgc = cbinfo.Length();
  auto cArgc = funcDef.argc;
  auto toCopy = jsArgc >= cArgc ? cArgc : jsArgc;
  auto toNull = jsArgc < cArgc ? cArgc - jsArgc : 0;

  Bscript::BObjectImp* funcRet;
  {
    Core::PolLock lck;
    if ( !uoexec.exists() )
      Napi::TypeError::New( env, "UOExecutor destroyed" ).ThrowAsJavaScriptException();

    for ( u32 i = 0; i < toCopy; ++i )
    {
      Bscript::BObjectRef convertedVal = Node::NodeObjectWrap::Wrap( env, cbinfo[i] );
      polmod->exec.fparams.emplace_back( convertedVal );
    }
    for ( u32 i = 0; i < toNull; i++ )
    {
      polmod->exec.fparams.emplace_back( new Bscript::UninitObject );
    }
    funcRet = polmod->execFunc( funcIdx );
    // cleanParams is protected... sooo
    polmod->exec.fparams.clear();
  }
  auto convertedFunctRet = NodeObjectWrap::Wrap( env, Bscript::BObjectRef( funcRet ) );
  return convertedFunctRet;
}
/**
 * Initialize the NodeModuleWrap module. It does not add any exports to
 * the JavaScript world (eg. no script should use `new NodeModuleWrap`).
 */
template <typename PolModule>
void NodeModuleWrap<PolModule>::Init( Napi::Env env, Napi::Object exports )
{
  NODELOG << "[modwrap] Initializing " << PolModule::modname << "\n";
  Napi::HandleScope scope( env );

  std::vector<Napi::ClassPropertyDescriptor<NodeModuleWrap<PolModule>>> props = {};
  int i = 0;
  for ( auto& iter : PolModule::function_table )
  {
    if ( iter.argc < UINT_MAX )
    {
      props.emplace_back( ObjectWrap<NodeModuleWrap<PolModule>>::InstanceMethod(
          iter.funcname.c_str(), &NodeModuleWrap<PolModule>::MethodWrapper, napi_default,
          (void*)( new int( i ) ) ) );
      NODELOG << "[modwrap] Module has function " << iter.funcname << " at " << i
              << " with argc = " << iter.argc << "\n";
    }
    else
    {
      NODELOG << "[modwrap] Module has unwrapped function " << iter.funcname << " at " << i
              << " (argc = UINT_MAX)\n";
    }
    ++i;
  }

  Napi::Function func =
      ObjectWrap<NodeModuleWrap<PolModule>>::DefineClass( env, PolModule::modname, props );
  constructor = Napi::Persistent( func );
  constructor.SuppressDestruct();
  exports.Set( PolModule::modname, func );
};

template <typename PolModule>
NodeModuleWrap<PolModule>::NodeModuleWrap( const Napi::CallbackInfo& info )
    : Napi::ObjectWrap<NodeModuleWrap<PolModule>>( info ), uoexec( nullptr )
{
  Napi::Env env = info.Env();
  Napi::HandleScope scope( env );

  size_t length = info.Length();

  if ( length <= 0 || !info[0].IsExternal() )
  {
    Napi::TypeError::New( env, "arg1=External expected" ).ThrowAsJavaScriptException();
  }

  uoexec = *( info[0].As<External<weak_ptr<Core::UOExecutor>>>().Data() );
  {
    Core::PolLock lck;
    if ( !uoexec.exists() )
    {
      Napi::TypeError::New( env, "UOExecutor destroyed" ).ThrowAsJavaScriptException();
    }

    polmod = static_cast<PolModule*>( uoexec.get_weakptr()->findModule( PolModule::modname ) );

    if ( polmod == nullptr )
    {
      polmod = new PolModule( *uoexec.get_weakptr() );
      uoexec.get_weakptr()->addModule( polmod );
    }
  }
}
}  // namespace Node
}  // namespace Pol


#endif