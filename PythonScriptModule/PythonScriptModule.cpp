// For conditions of distribution and use, see copyright notice in license.txt

/* NOTES FOR FURTHER CLEANUP: this module depends largely on the rexlogic module now. 
   early on it was the thing that implemented much of the functionality,
   and it also owns the scene which is the key for basic funcs like creating & getting scene entities.

   the idea of rexlogic, however, is to be a replaceable module that things in core don't generally depend on.
   the framework has developed so that most things can already be gotten without referring to it, like
   the default scene can now be gotten from Scene::ScenePtr scene = GetFramework()->GetDefaultWorldScene(); etc.
   if we could use the ECs without depending on the module itself, the dependency might be removed,
   and it would be more feasible to replace rexlogic with something and still have the py module work.

18:04 < antont> getting av and cam and doing session stuff like login&logout
                remains open
18:05 < Stinkfist> antont: pretty much, and after sempuki's re-factor,
                   login-logout should be possible to be done also (if i have
                   undestanded correctly)
18:05 < Stinkfist> without use of rexlogic
18:05 < sempuki> Stinkfist: yes
18:05 < antont> yep was thinking of that too, there'd be some session service
                thing or something by the new module

18:29 < antont> hm, there is also network sending code in rexlogic which we use
                from py, like void Primitive::SendRexPrimData(entity_id_t
                entityid)
18:31 < antont> iirc there was some issue that 'cause the data for those is not
                in rexlogic it makes the packets too, and not e.g. worldstream
                which doesn't know EC_OpenSimPrim
*/

#include "StableHeaders.h"
#include "DebugOperatorNew.h"

#include "PythonScriptModule.h"
#include "RexPythonQt.h"
#include "PythonScriptInstance.h"

#include "ModuleManager.h"
#include "EventManager.h"
#include "ServiceManager.h"
#include "ConsoleCommandServiceInterface.h"
#include "Input.h"
#include "RenderServiceInterface.h"
#include "PythonEngine.h"
#include "WorldStream.h"
#include "NetworkEvents.h"
#include "RealXtend/RexProtocolMsgIDs.h"
#include "RenderServiceInterface.h" //for getting rendering services, i.e. raycasts
#include "Inventory/InventorySkeleton.h"
#include "SceneManager.h"
#include "SceneEvents.h" //sending scene events after (placeable component) manipulation
#include "RexNetworkUtils.h"
#include "GenericMessageUtils.h"
#include "LoginServiceInterface.h"
#include "Frame.h"
#include "Console.h"
#include "ISoundService.h"
#include "NaaliUi.h"
#include "NaaliGraphicsView.h"

#include "Avatar/AvatarHandler.h"
#include "Avatar/AvatarControllable.h"

#include "RexLogicModule.h" //much of the api is here
#include "Camera/CameraControllable.h"
#include "Environment/Primitive.h"
#include "Environment/PrimGeometryUtils.h"
#include "EntityComponent/EC_AttachedSound.h"

//for CreateEntity. to move to an own file (after the possible prob with having api code in diff files is solved)
//#include "../OgreRenderingModule/EC_Mesh.h"
#include "Renderer.h"
#include "EC_Placeable.h"
#include "EC_OgreCamera.h"
#include "EC_Mesh.h"
#include "EC_OgreCustomObject.h"
#include "EC_OgreMovableTextOverlay.h"

#include "UiServiceInterface.h"
#include "UiProxyWidget.h"

#include "EC_OpenSimPrim.h"
#include "EC_3DCanvas.h"
#include "EC_Touchable.h"
#include "EC_Ruler.h"

//ECs declared by PythonScriptModule
#include "EC_DynamicComponent.h"
#include "EC_Script.h"

#include <PythonQt.h>

#include <QGroupBox> //just for testing addObject
#include <QtUiTools> //for .ui loading in testing
#include <QApplication>
#include <QGraphicsView>
#include <QWebView>
//#include <QDebug>

#include <MediaPlayerService.h>
#include <WorldBuildingServiceInterface.h>

#include "PythonQtScriptingConsole.h"

#include "KeyEvent.h"
#include "MouseEvent.h"

//#include <QDebug>

//==== Note py developers: MemoryLeakCheck must be the last include in order to make it work fully ====//
#include "MemoryLeakCheck.h"

namespace PythonScript
{
    std::string PythonScriptModule::type_name_static_ = "PythonScript";

    PythonScriptModule *PythonScriptModule::pythonScriptModuleInstance_ = 0;

    PythonScriptModule::PythonScriptModule()
    :IModule(type_name_static_),
    pmmModule(0), pmmDict(0), pmmClass(0), pmmInstance(0)
    {
        pythonqt_inited = false;
        inboundCategoryID_ = 0;
        inputeventcategoryid = 0;
        networkstate_category_id = 0;
        framework_category_id = 0;
    }

    PythonScriptModule::~PythonScriptModule()
    {
        pythonScriptModuleInstance_ = 0;
    }

    // virtual
    void PythonScriptModule::Load()
    {
        DECLARE_MODULE_EC(EC_DynamicComponent);
        DECLARE_MODULE_EC(EC_Script);
    }

    // virtual
    void PythonScriptModule::Unload()
    {
        pythonScriptModuleInstance_ = 0;
        input.reset();

        pmmModule = pmmDict = pmmClass = pmmInstance = 0;
        foreach(UiProxyWidget *proxy, proxyWidgets)
            SAFE_DELETE(proxy);
    }

    void PythonScriptModule::PostInitialize()
    {
        em_ = framework_->GetEventManager();
        
        // Reprioritize to be able to override behaviour
        em_->RegisterEventSubscriber(this, 105);

        // Get Framework category, so we can listen to its event about protocol module ready,
        // then we can subscribe to the other networking categories
        framework_category_id = em_->QueryEventCategory("Framework");

        // Input (OIS)
        inputeventcategoryid = em_->QueryEventCategory("Input");
        // Scene (SceneManager)
        scene_event_category_ = em_->QueryEventCategory("Scene");
        
        // Create a new input context with a default priority of 100.
        input = framework_->GetInput()->RegisterInputContext("PythonInput", 100);

        /* add events constants - now just the input events */
        //XXX move these to some submodule ('input'? .. better than 'constants'?)
        /*PyModule_AddIntConstant(apiModule, "MOVE_FORWARD_PRESSED", Input::Events::MOVE_FORWARD_PRESSED);
        PyModule_AddIntConstant(apiModule, "MOVE_FORWARD_RELEASED", Input::Events::MOVE_FORWARD_RELEASED);
        LogInfo("Added event constants.");*/

        /* TODO: add other categories and expose the hierarchy as py submodules or something,
        add registrating those (it's not (currently) mandatory),
        to the modules themselves, e.g. InputModule (currently the OIS thing but that is to change) */
        const Foundation::EventManager::EventMap &evmap = em_->GetEventMap();
        Foundation::EventManager::EventMap::const_iterator cat_iter = evmap.find(inputeventcategoryid);
        if (cat_iter != evmap.end())
        {
            std::map<event_id_t, std::string> evs = cat_iter->second;
            for (std::map<event_id_t, std::string>::iterator ev_iter = evs.begin();
                ev_iter != evs.end(); ++ev_iter)
            {
                /*std::stringstream ss;
                ss << ev_iter->first << " (id:" << ev_iter->second << ")";
                LogInfo(ss.str());*/
                PyModule_AddIntConstant(apiModule, ev_iter->second.c_str(), ev_iter->first);
            }
        }
        else
            LogInfo("No registered events in the input category.");

        /*for (Foundation::EventManager::EventMap::const_iterator iter = evmap[inputeventcategoryid].begin();
            iter != evmap[inputeventcategoryid].end(); ++iter)
        {
            std::stringstream ss;
            ss << iter->first << " (id:" << iter->second << ")";
            LogInfo(ss.str());
        }*/
        
        /* TODO perhaps should expose all categories, so any module would get it's events exposed automagically 
        const Foundation::EventManager::EventCategoryMap &categories = em.GetEventCategoryMap();
        for(Foundation::EventManager::EventCategoryMap::const_iterator iter = categories.begin();
            iter != categories.end(); ++iter)
        
            std::stringstream ss;
            ss << iter->first << " (id:" << iter->second << ")";

            treeiter->set_value(0, ss.str());
        } */

        if (!pmmClass)
        {
            LogError("PythonScriptModule::Initialize was not successful! PostInit not proceeding.");
            return;
        }
        //now that the event constants etc are there, can instanciate the manager which triggers the loading of components
        if (PyCallable_Check(pmmClass)) {
            pmmInstance = PyObject_CallObject(pmmClass, NULL); 
            LogInfo("Instanciated Py ModuleManager.");
        } else {
            LogError("Unable to create instance from class ModuleManager");
        }

        RegisterConsoleCommand(Console::CreateCommand(
            "PyExec", "Execute given code in the embedded Python interpreter. Usage: PyExec(mycodestring)", 
            Console::Bind(this, &PythonScriptModule::ConsoleRunString))); 
        /* NOTE: called 'exec' cause is similar to py shell builtin exec() func.
         * Also in the IPython shell 'run' refers to running an external file and not the given string
         */

        RegisterConsoleCommand(Console::CreateCommand(
            "PyLoad", "Execute a python file. PyLoad(mypymodule)", 
            Console::Bind(this, &PythonScriptModule::ConsoleRunFile))); 

        RegisterConsoleCommand(Console::CreateCommand(
            "PyReset", "Resets the Python interpreter - should free all it's memory, and clear all state.", 
            Console::Bind(this, &PythonScriptModule::ConsoleReset)));
    }

    bool PythonScriptModule::HandleEvent(event_category_id_t category_id, event_id_t event_id, IEventData* data)
    {    
        PyObject* value = NULL;

        //input events. 
        //another option for enabling py handlers for these would be to allow
        //implementing input state in py, see the AvatarController and CameraController in rexlogic
        /*if (category_id == inputeventcategoryid && event_id == Input::Events::INWORLD_CLICK)
        {
            Input::Events::Movement *movement = checked_static_cast<Input::Events::Movement*>(data);
            
            value = PyObject_CallMethod(pmmInstance, "MOUSE_INPUT_EVENT", "iiiii", event_id, movement->x_.abs_, movement->y_.abs_, movement->x_.rel_, movement->y_.rel_);
        }
        else*/ if (category_id == scene_event_category_)
        {
            if (event_id == Scene::Events::EVENT_SCENE_ADDED)
            {
                Scene::Events::SceneEventData* edata = checked_static_cast<Scene::Events::SceneEventData *>(data);
                value = PyObject_CallMethod(pmmInstance, "SCENE_ADDED", "s", edata->sceneName.c_str());

                const Scene::ScenePtr &scene = framework_->GetScene(edata->sceneName.c_str());
                assert(scene.get());
                if (scene)
                {
                    connect(scene.get(), SIGNAL(ComponentAdded(Scene::Entity*, IComponent*, AttributeChange::Type)),
                        SLOT(OnComponentAdded(Scene::Entity*, IComponent*)));
                    connect(scene.get(), SIGNAL(ComponentRemoved(Scene::Entity*, IComponent*, AttributeChange::Type)),
                        SLOT(OnComponentRemoved(Scene::Entity*, IComponent*)));
                }
            }

            /*
             only handles local modifications so far, needs a network refactorin of entity update events
             to get inbound network entity updates workin
            */
            if (event_id == Scene::Events::EVENT_ENTITY_UPDATED) //XXX remove this and handle with the new generic thing below?
            {
                Scene::Events::SceneEventData* edata = checked_static_cast<Scene::Events::SceneEventData *>(data);
                //LogInfo("Entity updated.");
                unsigned int ent_id = edata->localID;
                if (ent_id != 0)
                    value = PyObject_CallMethod(pmmInstance, "ENTITY_UPDATED", "I", ent_id);
            }
            //todo: add EVENT_ENTITY_DELETED so that e.g. editgui can keep on track in collaborative editing when objs it keeps refs disappear

            //for mediaurl handler
            else if (event_id == Scene::Events::EVENT_ENTITY_VISUALS_MODIFIED) 
            {
                Scene::Events::EntityEventData *entity_data = dynamic_cast<Scene::Events::EntityEventData*>(data);
                if (!entity_data)
                    return false;
                    
                Scene::EntityPtr entity = entity_data->entity;
                if (!entity)
                    return false;

                value = PyObject_CallMethod(pmmInstance, "ENTITY_VISUALS_MODIFIED", "I", entity->GetId());
            }

            //how to pass any event data?
            /*else
            {
                // Note: can't assume that all scene events will use this datatype!!!
                Scene::Events::SceneEventData* edata = dynamic_cast<Scene::Events::SceneEventData *>(data);
                if (edata)
                {
                    unsigned int ent_id = edata->localID;    
                    if (ent_id != 0)
                        value = PyObject_CallMethod(pmmInstance, "SCENE_EVENT", "iI", event_id, ent_id);
                }
            }*/            
        }
        else if (category_id == networkstate_category_id) // if (category_id == "NETWORK?") 
        {
            if (event_id == ProtocolUtilities::Events::EVENT_SERVER_CONNECTED)
            {
                value = PyObject_CallMethod(pmmInstance, "LOGIN_INFO", "i", event_id);

                // Save inventory skeleton for later use.
                ProtocolUtilities::AuthenticationEventData *auth = checked_static_cast<ProtocolUtilities::AuthenticationEventData *>(data);
                assert(auth);
                if (!auth)
                    return false;

                inventory = auth->inventorySkeleton;
            }
            else if (event_id == ProtocolUtilities::Events::EVENT_SERVER_DISCONNECTED)
            {
                value = PyObject_CallMethod(pmmInstance, "SERVER_DISCONNECTED", "i", event_id); //XXX useless to pass the id here - remove, but verify that all users are ported then
            }
        }
        else if (category_id == framework_category_id)
        {
            if  (event_id == Foundation::NETWORKING_REGISTERED)
            {
                inboundCategoryID_ = em_->QueryEventCategory("NetworkIn");
                networkstate_category_id = em_->QueryEventCategory("NetworkState");
            }
            else if (event_id == Foundation::WORLD_STREAM_READY)
            {
                ProtocolUtilities::WorldStreamReadyEvent *event_data = checked_static_cast<ProtocolUtilities::WorldStreamReadyEvent *>(data);
                if (event_data) {
                    worldstream = event_data->WorldStream;
                    value = PyObject_CallMethod(pmmInstance, "WORLD_STREAM_READY", "i", event_id);
                }
            }
        }

        //was for first receive chat test, when no module provided it, so handles net event directly
        /* got a crash with this now during login, when the viewer was also getting asset data etc.
           disabling the direct reading of network data here now to be on the safe side,
           this has always behaved correctly till now though (since march). --antont june 12th */

        else if (category_id == inboundCategoryID_)
        {
            using namespace ProtocolUtilities;
            NetworkEventInboundData *event_data = static_cast<NetworkEventInboundData *>(data);
            NetMsgID msgID = event_data->messageID;
            NetInMessage *msg = event_data->message;
            const NetMessageInfo *info = event_data->message->GetMessageInfo();
            //std::vector<ProtocolUtilities::NetMessageBlock> vec = info->blocks;

            //Vector3df data = event_data->message->GetData();
            //assert(info);
            const std::string str = info->name;
            unsigned int id = info->id;

            //testing if the unsigned int actually is the same NetMsgID, in this case RexNetMsgAgentAlertMessage == 0xffff0087
            //if (id == 0xff09) //RexNetMsgObjectProperties == 0xff09
            //    LogInfo("golly... it worked");

            if (id == RexNetMsgGenericMessage)
            {
                PyObject *stringlist = PyList_New(0);
                if (!stringlist)
                    return false;

                std::string cxxmsgname = ParseGenericMessageMethod(*msg);
                StringVector params = ParseGenericMessageParameters(*msg);

                for (uint i = 0; i < params.size(); ++i)
                {
                    std::string cxxs = params[i];
                    PyObject *pys = PyString_FromStringAndSize(cxxs.c_str(), cxxs.size());
                    if (!pys) 
                    {
                        //Py_DECREF(stringlist);
                        return false;
                    }
                    PyList_Append(stringlist, pys);
                    Py_DECREF(pys);
                }

                value = PyObject_CallMethod(pmmInstance, "GENERIC_MESSAGE", "sO", cxxmsgname.c_str(), stringlist);
            }
            /*else
            {
                value = PyObject_CallMethod(pmmInstance, "INBOUND_NETWORK", "Is", id, str.c_str());//, msgID, msg);
            }*/
            
            /*
            std::stringstream ss;
            ss << info->name << " received, " << ToString(msg->GetDataSize()) << " bytes.";
            //LogInfo(ss.str());

            switch(msgID)
            {
            case RexNetMsgChatFromSimulator:
                {
                std::stringstream ss;
                size_t bytes_read = 0;

                std::string name = (const char *)msg->ReadBuffer(&bytes_read);
                msg->SkipToFirstVariableByName("Message");
                std::string message = (const char *)msg->ReadBuffer(&bytes_read);
                
                //ss << "[" << GetLocalTimeString() << "] " << name << ": " << message << std::endl;
                //LogInfo(ss.str());
                //WriteToChatWindow(ss.str());
                //can readbuffer ever return null? should be checked if yes. XXX

                PyObject_CallMethod(pmmInstance, "RexNetMsgChatFromSimulator", "ss", name, message);

                break;
                }
            }*/
        }

        if (value)
        {
            if (PyObject_IsTrue(value))
            {
                //LogInfo("X_INPUT_EVENT returned True.");
                return true;  
            } 
            else 
            {
                //LogInfo("X_INPUT_EVENT returned False.");
                return false;
            }
        }
        return false;
    }

    Console::CommandResult PythonScriptModule::ConsoleRunString(const StringVector &params)
    {
        if (params.size() != 1)
            return Console::ResultFailure("Usage: PyExec(print 1 + 1)");
            //how to handle input like this? PyExec(print '1 + 1 = %d' % (1 + 1))");
            //probably better have separate py shell.
        engine_->RunString(QString::fromStdString(params[0]));

        return Console::ResultSuccess();
    }

    //void PythonScriptModule::x()
    //{
    //    Vector3df v1 = Vector3df();
    //    using Core;
    //    v2 = Vector3df();
    //    
    //    RexLogic::RexLogicModule *rexlogic_;
    //    rexlogic_ = dynamic_cast<RexLogic::RexLogicModule *>(framework_->GetModuleManager()->GetModule("RexLogic").lock().get());

    //    rexlogic_->GetServerConnection()->SendChatFromViewerPacket("x");

    //    rexlogic_->GetServerConnection()->IsConnected();
    //    rexlogic_->GetCameraControllable()->GetPitch();
    //    
    //    float newyaw = 0.1;
    //    //rexlogic_->GetAvatarControllable()->SetYaw(newyaw);
    //    rexlogic_->SetAvatarYaw(newyaw);
    //    //rexlogic_->GetAvatarControllable()->AddTime(0.1);
    //    //rexlogic_->GetAvatarControllable()->HandleInputEvent(0, NULL);
    //    
    //    //rexlogic_->GetAvatarControllable()->HandleAgentMovementComplete(Vector3(128, 128, 25), Vector3(129, 129, 24));
    //}

    Console::CommandResult PythonScriptModule::ConsoleRunFile(const StringVector &params)
    {
        if (params.size() != 1)
            return Console::ResultFailure("Usage: PyLoad(mypymodule) (to run mypymodule.py by importing it)");

        engine_->RunScript(QString::fromStdString(params[0]));
        return Console::ResultSuccess();
    }

    Console::CommandResult PythonScriptModule::ConsoleReset(const StringVector &params)
    {
        //engine_->Reset();
        Uninitialize(); //does also engine_->Uninitialize();
        Initialize();

        return Console::ResultSuccess();
    }

    // virtual 
    void PythonScriptModule::Uninitialize()
    {
        framework_->GetServiceManager()->UnregisterService(engine_);

        if (pmmInstance != NULL) //sometimes when devving it can be, when there was a bug - this helps to be able to reload it
            PyObject_CallMethod(pmmInstance, "exit", "");
        /*char** args = new char*[2]; //is this 2 'cause the latter terminates?
        std::string methodname = "exit";
        std::string paramtypes = ""; //"f"
        modulemanager->CallMethod2(methodname, paramtypes);*/

        engine_->Uninitialize();

        created_inputs_.clear();
        em_.reset();
        engine_.reset();
        inventory.reset();
    }
    
    // virtual
    void PythonScriptModule::Update(f64 frametime)
    {
        //XXX remove when/as the core has the fps limitter
        //engine_->RunString("import time; time.sleep(0.01);"); //a hack to save cpu now.

        // Somehow this causes extreme lag in consoleless mode         
        if (pmmInstance != NULL)
            PyObject_CallMethod(pmmInstance, "run", "f", frametime);
        
        /*char** args = new char*[2]; //is this 2 'cause the latter terminates?
        std::string methodname = "run";
        std::string paramtypes = "f";
        modulemanager->CallMethod2(methodname, paramtypes, 0.05); //;frametime);
        */        

        /* Mouse input special handling. InputModuleOIS has sending these as events commented out,
           This polling is copy-pasted from the InputHandler in RexLogicModule */
        //boost::shared_ptr<Input::InputServiceInterface> input = framework_->GetService<Input::InputServiceInterface>(Foundation::Service::ST_Input).lock();

        //XXX not ported to UImodule / OIS replacement yet
   //     boost::shared_ptr<Input::InputModuleOIS> input = framework_->GetModuleManager()->GetModule<Input::InputModuleOIS>(Foundation::Module::MT_Input).lock();
    }

    PythonScriptModule* PythonScriptModule::GetInstance()
    {
        assert(pythonScriptModuleInstance_);
        return pythonScriptModuleInstance_;
    }

     PyObject* PythonScriptModule::WrapQObject(QObject* qobj) const
     {
        return PythonQt::self()->priv()->wrapQObject(qobj);
    }

    OgreRenderer::Renderer* PythonScriptModule::GetRenderer() const
    {
        OgreRenderer::Renderer *renderer = framework_->GetService<OgreRenderer::Renderer>();
        if (renderer)
            return renderer;

        LogError("Renderer module not there?");
        return 0;
    }

    Foundation::WorldLogicInterface* PythonScriptModule::GetWorldLogic() const
    {
        Foundation::WorldLogicInterface *worldLogic = framework_->GetService<Foundation::WorldLogicInterface>();
        if (worldLogic)
            return worldLogic;

        LogError("WorldLogicInterface service not available in py GetWorldLogic");
        return 0;
    }

    Scene::SceneManager* PythonScriptModule::GetScene(const QString &name) const
    {
        Scene::ScenePtr scene = framework_->GetScene(name);
        if (scene)
            return scene.get();

        return 0;
    }

    void PythonScriptModule::RunJavascriptString(const QString &codestr, const QVariantMap &context)
    {
        using namespace Foundation;
        boost::shared_ptr<ScriptServiceInterface> js = framework_->GetService<ScriptServiceInterface>(Service::ST_JavascriptScripting).lock();
        if (js)
            js->RunString(codestr, context);
        else
            LogError("Javascript script service not available in py RunJavascriptString");
    }

    InputContext* PythonScriptModule::CreateInputContext(const QString &name, int priority)
    {
        InputContextPtr new_input = framework_->GetInput()->RegisterInputContext(name.toStdString().c_str(), priority);
        if (new_input)
        {
            LogDebug("Created new input context with name: " + name.toStdString());
            created_inputs_ << new_input; // Need to store these otherwise we get scoped ptr crash after return
            return new_input.get();
        }
        else
            return 0;
    }

    MediaPlayer::ServiceInterface* PythonScriptModule::GetMediaPlayerService() const
    {
        Foundation::Framework* framework = PythonScript::self()->GetFramework();
        if (!framework)
        {
            PythonScriptModule::LogCritical("Framework object doesn't exist!");
            return 0;
        }

        MediaPlayer::ServiceInterface *player_service = framework_->GetService<MediaPlayer::ServiceInterface>();
        if (player_service)
            return player_service;

        PythonScriptModule::LogError("Cannot find PlayerServiceInterface implementation.");
        return 0;
    }

    void PythonScriptModule::RemoveQtDynamicProperty(QObject* qobj, char* propname)
    {
        qobj->setProperty(propname, QVariant());
    }

    //this whole thing could be probably implemented in py now as well, but perhaps ok in c++ for speed
  QList<Scene::Entity*> PythonScriptModule::ApplyUICanvasToSubmeshesWithTexture(QWidget* qwidget_ptr, QObject* qobject_ptr, QString uuidstr, uint refresh_rate)
    {
        // Iterate the scene to find all submeshes that use this texture uuid
        QList<uint> submeshes_;
        QList<Scene::Entity*> affected_entitys_;

        if (!qwidget_ptr)
            return affected_entitys_;

        RexUUID texture_uuid = RexUUID();
        texture_uuid.FromString(uuidstr.toStdString());

        Scene::ScenePtr scene = PythonScript::self()->GetFramework()->GetDefaultWorldScene();
        if (!scene)
        {
            //PyErr_SetString(PyExc_RuntimeError, "Default scene is not there in GetEntityMatindicesWithTexture.");
            return affected_entitys_;
        }

        Foundation::WorldLogicInterface *worldLogic = PythonScript::self()->GetWorldLogic();
        /* was wrong way, how did this work? if (!worldLogic)
        {
          //PyErr_SetString(PyExc_RuntimeError, "Could not get world logic.");
          return;
          }*/

        Scene::EntityList prims = scene->GetEntitiesWithComponent("EC_OpenSimPrim");
        foreach(Scene::EntityPtr e, prims)
        {
            submeshes_.clear();

            EC_OpenSimPrim *prim = e->GetComponent<EC_OpenSimPrim>().get();

            if (prim->DrawType == RexTypes::DRAWTYPE_MESH || prim->DrawType == RexTypes::DRAWTYPE_PRIM)
            {
                ComponentPtr mesh = e->GetComponent(EC_Mesh::TypeNameStatic());
                ComponentPtr custom_object = e->GetComponent(EC_OgreCustomObject::TypeNameStatic());
            
                EC_Mesh *meshptr = 0;
                EC_OgreCustomObject *custom_object_ptr = 0;

                if (mesh) 
                {
                    meshptr = checked_static_cast<EC_Mesh*>(mesh.get());
                    if (!meshptr)
                        continue;
                    if (!meshptr->GetEntity())
                        continue;
                }
                else if (custom_object)
                {
                    custom_object_ptr = checked_static_cast<EC_OgreCustomObject*>(custom_object.get());
                    if (!custom_object_ptr)
                        continue;
                    if (!custom_object_ptr->GetEntity())
                      continue;
                }
                else
                    continue;
            
                // Iterate mesh materials map
                if (meshptr)
                {
                    MaterialMap material_map = prim->Materials;
                    MaterialMap::const_iterator i = material_map.begin();
                    while (i != material_map.end())
                    {
                        // Store sumbeshes to list where we want to apply the new widget as texture
                        uint submesh_id = i->first;
                        if ((i->second.Type == RexTypes::RexAT_Texture) && (i->second.asset_id.compare(texture_uuid.ToString()) == 0))
                            submeshes_.append(submesh_id);
                        ++i;
                    }
                }
                // Iterate custom object texture map
                else if (custom_object_ptr && prim->PrimTextures.size() > 0 )
                {
                    TextureMap texture_map = prim->PrimTextures;
                    TextureMap::const_iterator i = texture_map.begin();

                    while (i != texture_map.end()) /// @todo This causes unresolved crash in some cases!
                    {
                        uint submesh_id = i->first;
                        if (i->second == texture_uuid.ToString())
                            submeshes_.append(submesh_id);
                        ++i;
                    }
                }
                else
                    continue;

                if (submeshes_.size() > 0)
                {
                    PythonScriptModule::Add3DCanvasComponents(e.get(), qwidget_ptr, submeshes_, refresh_rate);
                    affected_entitys_.append(e.get());
                }
            }
        }

        return affected_entitys_;
    }

    void PythonScriptModule::LoadScript(const QString &filename)
    {
        EC_Script *script = dynamic_cast<EC_Script *>(sender());
        if (!script)
            return;

        if (script->type.Get() != "py")
            return;

        PythonScriptInstance *pyInstance = new PythonScriptInstance(script->scriptRef.Get(), script->GetParentEntity());
        script->SetScriptInstance(pyInstance);
        if (script->runOnLoad.Get())
            script->Run();
    }

    void PythonScriptModule::OnComponentAdded(Scene::Entity *entity, IComponent *component)
    {
        if (component->TypeName() == EC_Script::TypeNameStatic())
        {
            EC_Script *script = static_cast<EC_Script *>(component);
            connect(script, SIGNAL(ScriptRefChanged(const QString &)), SLOT(LoadScript(const QString &)));
        }
    }

    void PythonScriptModule::OnComponentRemoved(Scene::Entity *entity, IComponent *component)
    {
    }

    PythonQtScriptingConsole* PythonScriptModule::CreateConsole()
    {
        PythonQtScriptingConsole* pythonqtconsole = new PythonQtScriptingConsole(NULL, PythonQt::self()->getMainModule());
        return pythonqtconsole;
    }
}

extern "C" void POCO_LIBRARY_API SetProfiler(Foundation::Profiler *profiler);
void SetProfiler(Foundation::Profiler *profiler)
{
    Foundation::ProfilerSection::SetProfiler(profiler);
}

using namespace PythonScript;

POCO_BEGIN_MANIFEST(IModule)
    POCO_EXPORT_CLASS(PythonScriptModule)
POCO_END_MANIFEST

#ifdef __cplusplus
extern "C"
#endif

/* API calls exposed to py. 
will probably be wrapping the actual modules in separate files,
but first test now here. also will use boostpy or something, but now first by hand */
PyObject* SendChat(PyObject *self, PyObject *args)
{
    const char* msg;

    if(!PyArg_ParseTuple(args, "s", &msg))
    {
        PyErr_SetString(PyExc_ValueError, "param should be a string.");
        return NULL;
    }

    //move decl to .h and getting to Initialize (see NetTEstLogicModule::Initialize)
    //if this kind of usage, i.e. getting the logic module for the api, is to remain.
    if (PythonScript::self()->worldstream)
        PythonScript::self()->worldstream->SendChatFromViewerPacket(msg);
    //rexlogic_->GetServerConnection()->IsConnected();
    //float newyaw = 0.1;
    //rexlogic_->GetAvatarControllable()->SetYaw(newyaw);
    //rexlogic_->GetCameraControllable()->GetPitch();
    //rexlogic_->GetAvatarControllable()->HandleAgentMovementComplete(Vector3(128, 128, 25), Vector3(129, 129, 24));

    Py_RETURN_TRUE;
}

static PyObject* SetAvatarRotation(PyObject *self, PyObject *args)
{
    RexLogic::RexLogicModule *rexlogic = PythonScript::self()->GetFramework()->GetModule<RexLogic::RexLogicModule>();
    if (rexlogic)
    {
        float x, y, z, w;

        if(!PyArg_ParseTuple(args, "ffff", &x, &y, &z, &w))
        {
            PyErr_SetString(PyExc_ValueError, "Value error, need x, y, z, w params");
            return NULL;
        }
        std::cout << "Sending newrot..." << std::endl;
        Quaternion newrot(x, y, z, w); //seriously, is this how constructing a quat works!?
        rexlogic->SetAvatarRotation(newrot);
    }

    Py_RETURN_NONE;
}

//returns the entity(id) at the position (x, y), if nothing there, returns None
//\todo XXX renderer is a qobject now, rc should be made a slot there and this removed.
static PyObject* RayCast(PyObject *self, PyObject *args)
{
    uint x, y;
    
    if(!PyArg_ParseTuple(args, "II", &x, &y)){
        PyErr_SetString(PyExc_ValueError, "Raycasting failed due to ValueError, needs (x, y) values.");
        return NULL;   
    }

    Foundation::RenderServiceInterface *render = PythonScript::self()->GetFramework()->GetService<Foundation::RenderServiceInterface>();
    if (render)
    {
        //Scene::Entity *entity = render->Raycast(x, y).entity_;
        Foundation::RaycastResult result = render->Raycast(x, y);

        if (result.entity_)
            return Py_BuildValue("IfffIff", result.entity_->GetId(), result.pos_.x, result.pos_.y, result.pos_.z,
                result.submesh_, float(result.u_), float(result.v_));
        else
            Py_RETURN_NONE;
    }

    Py_RETURN_NONE;
}

static PyObject* GetQWorldBuildingHandler(PyObject *self)
{
    Foundation::WorldBuildingServiceInterface *wb =  PythonScript::self()->GetFramework()->GetService<Foundation::WorldBuildingServiceInterface>();
    if (wb)
        return PythonScriptModule::GetInstance()->WrapQObject(wb->GetPythonHandler());
    else
        Py_RETURN_NONE;
}

static PyObject* TakeScreenshot(PyObject *self, PyObject *args)
{
    const char* filePath;
    const char* fileName;

    if(!PyArg_ParseTuple(args, "ss", &filePath, &fileName))
        PyErr_SetString(PyExc_ValueError, "Getting the filepath and filename failed.");

    OgreRenderer::Renderer *renderer = PythonScript::self()->GetFramework()->GetService<OgreRenderer::Renderer>();
    if (renderer)
    {
        //std::cout << "Screenshot in PYSM ... " << std::endl;
        renderer->TakeScreenshot(filePath, fileName);
    }
    else
        std::cout << "Could not retrieve Ogre renderer." << std::endl;

    Py_RETURN_NONE;
}

static PyObject* SwitchCameraState(PyObject *self)
{
    RexLogic::RexLogicModule *rexlogic = PythonScript::self()->GetFramework()->GetModule<RexLogic::RexLogicModule>();
    rexlogic->SwitchCameraState();
    Py_RETURN_NONE;
}

static PyObject* SendEvent(PyObject *self, PyObject *args)
{
    std::cout << "PySendEvent" << std::endl;
    unsigned int event_id_int;//, ent_id_int = 0;
    Foundation::Framework *framework_ = PythonScript::self()->GetFramework();//PythonScript::staticframework;
    //entity_id_t ent_id;
    event_id_t event_id;

    if(!PyArg_ParseTuple(args, "i", &event_id_int))//, &ent_id_int))
    {
        PyErr_SetString(PyExc_ValueError, "Getting an event id failed, param should be an integer.");
        return NULL;   
    }
    
    //ent_id = (entity_id_t) ent_id_int;
    event_id = (event_id_t) event_id_int;
    event_category_id_t event_category = framework_->GetEventManager()->QueryEventCategory("Input");
    if (event_id == InputEvents::SWITCH_CAMERA_STATE) 
    {
        std::cout << "switch camera state gotten!" << std::endl;
        framework_->GetEventManager()->SendEvent(event_category, event_id, NULL);//&event_data);
    } 
    else
        std::cout << "failed..." << std::endl;

    Py_RETURN_TRUE;
}

PyObject* GetEntityByUUID(PyObject *self, PyObject *args)
{
    char* uuidstr;
    if(!PyArg_ParseTuple(args, "s", &uuidstr))
    {
        PyErr_SetString(PyExc_ValueError, "Getting an entity by UUID failed, param should be a string.");
        return NULL;
    }

    RexUUID ruuid = RexUUID();
    ruuid.FromString(std::string(uuidstr));

    PythonScriptModule *owner = PythonScriptModule::GetInstance();

    RexLogic::RexLogicModule *rexlogic_ = PythonScript::self()->GetFramework()->GetModule<RexLogic::RexLogicModule>();
    if (rexlogic_)
    {
        //PythonScript::self()->LogInfo("Getting prim with UUID:" + ruuid.ToString());
        Scene::EntityPtr entity = rexlogic_->GetPrimEntity(ruuid);
        if (entity)
        {
            return PythonScriptModule::GetInstance()->WrapQObject(entity.get());
        }
        else
        {
            //PyErr_SetString(PyExc_RuntimeError, "Entity found by UUID is not in default scene?");
            //return NULL;   
            Py_RETURN_NONE; //not there yet, when still loading?
        }
    }
    else
    {
        PyErr_SetString(PyExc_RuntimeError, "RexLogic module not there?");
        return NULL;   
    }
}

PyObject* CheckSceneForTexture(PyObject* self, PyObject* args)
{
    const char* uuidstr;
    if(!PyArg_ParseTuple(args, "s", &uuidstr))
        return NULL;

    RexUUID texture_uuid(uuidstr);

    /// Get scene
    const Scene::ScenePtr &scene = PythonScript::self()->GetFramework()->GetDefaultWorldScene();
    if (!scene)
    {
        PyErr_SetString(PyExc_RuntimeError, "Default scene is not there in GetEntityMatindicesWithTexture.");
        return NULL;
    }

    // Iterate the scene to find all submeshes that use this texture uuid
    QList<uint> submeshes_;
    bool submeshes_found_ = false;
    Scene::EntityList prims = scene->GetEntitiesWithComponent("EC_OpenSimPrim");
    foreach(Scene::EntityPtr e, prims)
    {
        Scene::Entity &entity = *e.get();
        submeshes_.clear();

        EC_OpenSimPrim &prim = *entity.GetComponent<EC_OpenSimPrim>();

        if (prim.DrawType == RexTypes::DRAWTYPE_MESH || prim.DrawType == RexTypes::DRAWTYPE_PRIM)
        {
            ComponentPtr mesh = entity.GetComponent(EC_Mesh::TypeNameStatic());
            ComponentPtr custom_object = entity.GetComponent(EC_OgreCustomObject::TypeNameStatic());

            EC_Mesh *meshptr = 0;
            EC_OgreCustomObject *custom_object_ptr = 0;

            if (mesh)
            {
                meshptr = checked_static_cast<EC_Mesh*>(mesh.get());
                if (!meshptr)
                    continue;
                if (!meshptr->GetEntity())
                    continue;
            }
            else if (custom_object)
            {
                custom_object_ptr = checked_static_cast<EC_OgreCustomObject*>(custom_object.get());
                if (!custom_object_ptr)
                    continue;
                if (!custom_object_ptr->GetEntity())
                    continue;
            }
            else
                continue;
            
            // Iterate mesh materials map
            if (meshptr)
            {
                MaterialMap material_map = prim.Materials;
                MaterialMap::const_iterator i = material_map.begin();
                while (i != material_map.end())
                {
                    // Store sumbeshes to list where we want to apply the new widget as texture
                    uint submesh_id = i->first;
                    if ((i->second.Type == RexTypes::RexAT_Texture))
                    {
                        if ((i->second.asset_id.compare(texture_uuid.ToString()) == 0))
                        {
                            submeshes_.append(submesh_id);
                        }
                        else
                        {
                            // Url asset id check for containing texture UUID
                            QString q_asset_id(i->second.asset_id.c_str());
                            if (q_asset_id.contains(texture_uuid.ToString().c_str()))
                                submeshes_.append(submesh_id);
                        }
                    }
                    ++i;
                }
            }
            // Iterate custom object texture map
            else if (custom_object_ptr && prim.PrimTextures.size() > 0 )
            {
                TextureMap texture_map = prim.PrimTextures;
                TextureMap::const_iterator i = texture_map.begin();

                while (i != texture_map.end()) /// @todo This causes unresolved crash in some cases!
                {
                    uint submesh_id = i->first;
                    if (i->second == texture_uuid.ToString())
                        submeshes_.append(submesh_id);
                    ++i;
                }
            }
            else
                continue;

            if (submeshes_.size() > 0)
                Py_RETURN_TRUE; // No need to iterate further if any submeshes were found
        }
    }

    if (submeshes_found_)
        Py_RETURN_TRUE;
    else
        Py_RETURN_FALSE;
}

PyObject* ApplyUICanvasToSubmeshes(PyObject* self, PyObject* args)
{
    uint ent_id_int;
    PyObject* py_submeshes;
    PyObject* py_qwidget;
    uint refresh_rate;

    if(!PyArg_ParseTuple(args, "IOOI", &ent_id_int, &py_submeshes, &py_qwidget, &refresh_rate))
        return NULL;

    // Get entity pointer
    RexLogic::RexLogicModule *rexlogicmodule_ = dynamic_cast<RexLogic::RexLogicModule *>(PythonScript::self()->GetFramework()->GetModuleManager()->GetModule("RexLogic").lock().get());
    Scene::EntityPtr primentity = rexlogicmodule_->GetPrimEntity((entity_id_t)ent_id_int);
    
    if (!primentity) 
        Py_RETURN_NONE;
    if (!PyList_Check(py_submeshes))
        return NULL;
    if (!PyObject_TypeCheck(py_qwidget, &PythonQtInstanceWrapper_Type))
        return NULL;

    PythonQtInstanceWrapper* wrapped_qwidget = (PythonQtInstanceWrapper*)py_qwidget;
    QObject* qobject_ptr = wrapped_qwidget->_obj;
    QWidget* qwidget_ptr = (QWidget*)qobject_ptr;

    if (!qwidget_ptr)
        return NULL;

    // Convert a python list to a std::vector.
    // TODO: Make a util function for this? - Jonne Nauha
    QList<uint> submeshes_;
    PyObject* py_iter;
    PyObject* item;
    py_iter = PyObject_GetIter(py_submeshes);
    while (item = PyIter_Next(py_iter))
    {
        PyObject* py_int;
        py_int = PyNumber_Int(item);
        if (!item)
        {
            Py_DECREF(py_iter);
            Py_DECREF(item);
            PyErr_SetString(PyExc_ValueError, "Submesh indexes for applying uicanvases must be integers.");
            return NULL;
        }
        submeshes_.append((uint)PyInt_AsLong(py_int));
        Py_DECREF(item);
    }
    Py_DECREF(py_iter);

    PythonScriptModule::Add3DCanvasComponents(primentity.get(), qwidget_ptr, submeshes_, refresh_rate);

    Py_RETURN_NONE;
}

//XXX \todo remove and use the generic component adding mechanism from core directly. remove (canvas &) touchable deps from py module then
void PythonScriptModule::Add3DCanvasComponents(Scene::Entity *entity, QWidget *widget, const QList<uint> &submeshes, int refresh_rate)
{
    if (submeshes.isEmpty())
        return;
    
    // Always create new EC_3DCanvas component
    EC_3DCanvas *ec_canvas = entity->GetComponent<EC_3DCanvas>().get();
    if (ec_canvas)
        entity->RemoveComponent(entity->GetComponent<EC_3DCanvas>());    

    // Add the component
    entity->AddComponent(PythonScript::self()->GetFramework()->GetComponentManager()->CreateComponent(EC_3DCanvas::TypeNameStatic()));
    ec_canvas = entity->GetComponent<EC_3DCanvas>().get();

    if (ec_canvas)
    {
        // Setup the component
        ec_canvas->Setup(widget, submeshes, refresh_rate);
        ec_canvas->Start();
    }

    // Touchable 
    EC_Touchable *ec_touchable = entity->GetComponent<EC_Touchable>().get();
    if (!ec_touchable)
    {
        entity->AddComponent(PythonScript::self()->GetFramework()->GetComponentManager()->CreateComponent(EC_Touchable::TypeNameStatic()));
        ec_touchable = entity->GetComponent<EC_Touchable>().get();
        ec_touchable->SetNetworkSyncEnabled(false);
    }
    if (ec_touchable)
    {
        ec_touchable->highlightOnHover.Set(false, AttributeChange::LocalOnly);
        ec_touchable->hoverCursor.Set(Qt::PointingHandCursor, AttributeChange::LocalOnly);
    }
}

PyObject* GetSubmeshesWithTexture(PyObject* self, PyObject* args)
{
    // Read params from py: entid as uint, textureuuid as string
    unsigned int ent_id_int;
    const char* uuidstr;
    entity_id_t ent_id;

    if(!PyArg_ParseTuple(args, "Is", &ent_id_int, &uuidstr))
        return NULL;

    ent_id = (entity_id_t)ent_id_int;
    RexUUID texture_uuid(uuidstr);

    Foundation::WorldLogicInterface *worldLogic = PythonScript::self()->GetFramework()->GetService<Foundation::WorldLogicInterface>();
    if (!worldLogic)
        Py_RETURN_NONE;

    Scene::EntityPtr primentity = worldLogic->GetEntityWithComponent(ent_id, EC_OpenSimPrim::TypeNameStatic());
    if (!primentity)
        Py_RETURN_NONE;

    EC_OpenSimPrim &prim = *primentity->GetComponent<EC_OpenSimPrim>();
    if (prim.DrawType == RexTypes::DRAWTYPE_MESH || prim.DrawType == RexTypes::DRAWTYPE_PRIM)
    {
        QList<uint> submeshes_;
        ComponentPtr mesh = primentity->GetComponent(EC_Mesh::TypeNameStatic());
        ComponentPtr custom_object = primentity->GetComponent(EC_OgreCustomObject::TypeNameStatic());

        EC_Mesh *meshptr = 0;
        EC_OgreCustomObject *custom_object_ptr = 0;

        if (mesh)
        {
            meshptr = checked_static_cast<EC_Mesh*>(mesh.get());
            if (!meshptr)
                Py_RETURN_NONE;
            if (!meshptr->GetEntity())
                Py_RETURN_NONE;
        }
        else if (custom_object)
        {
            custom_object_ptr = checked_static_cast<EC_OgreCustomObject*>(custom_object.get());
            if (!custom_object_ptr)
                Py_RETURN_NONE;
            if (!custom_object_ptr->GetEntity())
                Py_RETURN_NONE;
        }
        else
            Py_RETURN_NONE;
        
        // Iterate mesh materials map
        if (meshptr)
        {
            MaterialMap material_map = prim.Materials;
            MaterialMap::const_iterator i = material_map.begin();
            while (i != material_map.end())
            {
                // Store sumbeshes to list where we want to apply the new widget as texture
                uint submesh_id = i->first;
                if ((i->second.Type == RexTypes::RexAT_Texture) && (i->second.asset_id.compare(texture_uuid.ToString()) == 0))
                    submeshes_.append(submesh_id);
                ++i;
            }
        }
        // Iterate custom object texture map
        else if (custom_object_ptr)
        {
            //! todo: fix this shit, find a smarter way than regenerating the prims multiple times without optimisation
            //! so that we get the correct submesh indexes from it.
            Ogre::ManualObject* manual = RexLogic::CreatePrimGeometry(PythonScript::self()->GetFramework(), prim, false);
            custom_object_ptr->CommitChanges(manual);

            TextureMap texture_map = prim.PrimTextures;
            TextureMap::const_iterator i = texture_map.begin();
            if (i == texture_map.end())
            {
                if (prim.getPrimDefaultTextureID() == QString(texture_uuid.ToString().c_str()))
                    for (uint submesh = 0; submesh < 6; ++submesh)
                        submeshes_.append(submesh);
            }
            else
            {
                while (i != texture_map.end())
                {
                    uint submesh_id = i->first;
                    if (i->second.compare(texture_uuid.ToString()) == 0)
                        submeshes_.append(submesh_id);
                    ++i;
                }
            }
        }
        else
            Py_RETURN_NONE;

        if (submeshes_.count() > 0)
        {
            PyObject* py_submeshes = PyList_New(submeshes_.size());
            int i = 0;
            foreach(uint submesh, submeshes_)
            {
                PyList_SET_ITEM(py_submeshes, i, Py_BuildValue("I", submesh));
                ++i;
            }
            return py_submeshes;
        }
    }

    Py_RETURN_NONE;
}

PyObject* GetApplicationDataDirectory(PyObject *self)
{
    PythonScriptModule* module = PythonScriptModule::GetInstance();
    std::string cache_path = module->GetFramework()->GetPlatform()->GetApplicationDataDirectory();
    //PyString_New
    return PyString_FromString(cache_path.c_str());
    //return QString(cache_path.c_str());
}

//XXX remove, use SceneManager
PyObject* RemoveEntity(PyObject *self, PyObject *value)
{
    int ent_id;
    if(!PyArg_ParseTuple(value, "i", &ent_id))
    {
        PyErr_SetString(PyExc_ValueError, "id must be int"); //XXX change the exception
        return NULL;
    }
    PythonScriptModule *owner = PythonScriptModule::GetInstance();
    Scene::ScenePtr scene = owner->GetScenePtr();
    if (!scene){ //XXX enable the check || !rexlogicmodule_->GetFramework()->GetComponentManager()->CanCreate(EC_Placeable::TypeNameStatic()))
        PyErr_SetString(PyExc_RuntimeError, "Default scene is not there in RemoveEntity.");
        return NULL;   
    }
    scene->RemoveEntity(ent_id);
    //PyErr_SetString(PyExc_ValueError, "no error.");
    Py_RETURN_NONE;
}

/*
//camera zoom - send an event like logic CameraControllable does:
            CameraZoomEvent event_data;
            //event_data.entity = entity_.lock(); // no entity for camera, :( -cm
            event_data.amount = checked_static_cast<Input::Events::SingleAxisMovement*>(data)->z_.rel_;
            //if (event_data.entity) // only send the event if we have an existing entity, no point otherwise
            framework_->GetEventManager()->SendEvent(action_event_category_, RexTypes::Actions::Zoom, &event_data);
*/

//XXX logic CameraControllable has GetPitch, perhaps should have SetPitch too
PyObject* SetCameraYawPitch(PyObject *self, PyObject *args) 
{
    float newyaw, newpitch;
    float y, p;
    if(!PyArg_ParseTuple(args, "ff", &y, &p)) {
        PyErr_SetString(PyExc_ValueError, "New camera yaw and pitch expected as float, float.");
        return NULL;
    }
    newyaw = (float) y;
    newpitch = (float) p;

    //boost::shared_ptr<OgreRenderer::Renderer> renderer = PythonScript::staticframework->GetServiceManager()->GetService<OgreRenderer::Renderer>(Foundation::Service::ST_Renderer).lock();
    RexLogic::RexLogicModule *rexlogic = PythonScript::self()->GetFramework()->GetModule<RexLogic::RexLogicModule>();
    if (rexlogic)
    {
        //boost::shared_ptr<RexLogic::CameraControllable> cam = rexlogic_->GetCameraControllable();
        //cam->HandleInputEvent(PythonScript::PythonScriptModule::inputeventcategoryid, &x);
        //cam->AddTime((float) 0.1);
        //cam->SetPitch(p); //have a linking prob with this
        rexlogic->SetCameraYawPitch(y, p);
    }
    
    //was with renderer, worked but required overriding rexlogic :p
    //{
    //    Ogre::Camera *camera = renderer->GetCurrentCamera();
    //    camera->yaw(Ogre::Radian(newyaw));
    //    camera->pitch(Ogre::Radian(newpitch));
    //}
    else
    {
        PyErr_SetString(PyExc_RuntimeError, "No logic module, no cameracontrollable.");
        return NULL;
    }

    Py_RETURN_NONE;
}

PyObject* GetCameraYawPitch(PyObject *self, PyObject *args) 
{
    float yaw, pitch;
    RexLogic::RexLogicModule *rexlogic = PythonScript::self()->GetFramework()->GetModule<RexLogic::RexLogicModule>();
    if (rexlogic)
    {
        boost::shared_ptr<RexLogic::CameraControllable> cam = rexlogic->GetCameraControllable();
        pitch = cam->GetPitch();
        yaw = 0; //XXX not implemented yet (?)

        return Py_BuildValue("ff", (float)yaw, float(pitch));
    }
    //else - no logic module. can that ever happen?)
    return NULL; //rises py exception
}

PyObject* PyLogInfo(PyObject *self, PyObject *args) 
{
    const char* message;    
    if(!PyArg_ParseTuple(args, "s", &message))
    {
        PyErr_SetString(PyExc_ValueError, "Needs a string.");
        return NULL;
    }
    PythonScript::self()->LogInfo(message);
    Py_RETURN_NONE;
}

PyObject* PyLogDebug(PyObject *self, PyObject *args) 
{
    const char* message;
    if(!PyArg_ParseTuple(args, "s", &message))
    {
        PyErr_SetString(PyExc_ValueError, "Needs a string.");
        return NULL;
    }
    PythonScript::self()->LogDebug(message);
    Py_RETURN_NONE;
}

PyObject* PyLogWarning(PyObject *self, PyObject *args) 
{
    const char* message;
    if(!PyArg_ParseTuple(args, "s", &message))
    {
        PyErr_SetString(PyExc_ValueError, "Needs a string.");
        return NULL;
    }
    PythonScript::self()->LogWarning(message);
    Py_RETURN_NONE;
}

PyObject* PyLogError(PyObject *self, PyObject *args) 
{
    const char* message;
    if(!PyArg_ParseTuple(args, "s", &message))
    {
        PyErr_SetString(PyExc_ValueError, "Needs a string.");
        return NULL;
    }
    PythonScript::self()->LogError(message);
    
    Py_RETURN_NONE;
}

PyObject* SetAvatarYaw(PyObject *self, PyObject *args)
{
    float newyaw;

    float y;
    if(!PyArg_ParseTuple(args, "f", &y)) {
        PyErr_SetString(PyExc_ValueError, "New avatar yaw expected as float.");
        return NULL;
    }
    newyaw = (float) y;

    RexLogic::RexLogicModule *rexlogic = PythonScript::self()->GetFramework()->GetModule<RexLogic::RexLogicModule>();
    if (rexlogic)
    {
        //rexlogic_->GetServerConnection()->IsConnected();
        //had linking problems with these, hopefully can be solved somehow easily.
        //rexlogic_->GetAvatarControllable()->SetYaw(newyaw);
        //boost::shared_ptr<RexLogic::AvatarControllable> avc = rexlogic_->GetAvatarControllable();
        //avc->SetYaw(newyaw);
        //f64 t = (f64) 0.01;
        //avc->AddTime(t);
        //rexlogic_->GetAvatarControllable()->HandleAgentMovementComplete(Vector3(128, 128, 25), Vector3(129, 129, 24));
        rexlogic->SetAvatarYaw(newyaw);
    }
    
    else
    {
        PyErr_SetString(PyExc_RuntimeError, "No logic module, no AvatarControllable.");
        return NULL;
    }

    Py_RETURN_NONE;
}

//PyObject* CreateCanvas(PyObject *self, PyObject *args)
//{        
//    if (!PythonScript::self()->GetFramework())//PythonScript::staticframework)
//    {
//        //std::cout << "Oh crap staticframework is not there! (py)" << std::endl;
//        PythonScript::self()->LogInfo("PythonScript's framework is not present!");
//        return NULL;
//    }
//
//    int imode;
//
//    if(!PyArg_ParseTuple(args, "i", &imode))
//    {
//        PyErr_SetString(PyExc_ValueError, "Getting the mode failed, need 0 / 1");
//        return NULL;   
//    }
//    
//    boost::shared_ptr<QtUI::QtModule> qt_module = PythonScript::self()->GetFramework()->GetModuleManager()->GetModule<QtUI::QtModule>(Foundation::Module::MT_Gui).lock();
//    boost::shared_ptr<QtUI::UICanvas> canvas_;
//    
//    if ( qt_module.get() == 0)
//        return NULL;
//    
//    QtUI::UICanvas::DisplayMode rMode = (QtUI::UICanvas::DisplayMode) imode;
//    canvas_ = qt_module->CreateCanvas(rMode).lock();
//
//    QtUI::UICanvas* qcanvas = canvas_.get();
//    
//    PyObject* can = PythonQt::self()->wrapQObject(qcanvas);
//
//    return can;
//}

PyObject* CreateUiProxyWidget(PyObject* self, PyObject *args)
{
    UiServiceInterface *ui = PythonScript::self()->GetFramework()->GetService<UiServiceInterface>();
    if (!ui)
    {
        // If this occurs, we're most probably operating in headless mode.
        //XXX perhaps should not be an error, 'cause some things should just work in headless without complaining
        PyErr_SetString(PyExc_RuntimeError, "UI service is missing.");
        return NULL;
    }

//    if(!PyArg_ParseTuple(args, "OO", &pywidget, &pyuiprops))
    PyObject* pywidget;
    if (!PyArg_ParseTuple(args, "O", &pywidget))
        return NULL;

    if (!PyObject_TypeCheck(pywidget, &PythonQtInstanceWrapper_Type))
        return NULL;

    PythonQtInstanceWrapper* wrapped_widget = (PythonQtInstanceWrapper*)pywidget;
    QObject* widget_ptr = wrapped_widget->_obj;
    QWidget* widget = (QWidget*)widget_ptr;

    UiProxyWidget* proxy = new UiProxyWidget(widget);
    PythonScriptModule::GetInstance()->proxyWidgets << proxy;
    return PythonScriptModule::GetInstance()->WrapQObject(proxy);
}

PyObject* DisconnectUIViewSignals(PyObject *self)
{
    QGraphicsView *view = PythonScript::self()->GetFramework()->Ui()->GraphicsView();
    view->disconnect();
    Py_RETURN_NONE;
}

PyObject* GetUIView(PyObject *self)
{
    return PythonScriptModule::GetInstance()->WrapQObject(PythonScript::self()->GetFramework()->Ui()->GraphicsView());
}

PyObject* GetRexLogic(PyObject *self)
{
    RexLogic::RexLogicModule *rexlogic = PythonScript::self()->GetFramework()->GetModule<RexLogic::RexLogicModule>();
    if (rexlogic)
        return PythonScriptModule::GetInstance()->WrapQObject(rexlogic);
    PyErr_SetString(PyExc_RuntimeError, "RexLogic is missing.");
    return NULL;
}

PyObject* GetServerConnection(PyObject *self)
{
    return PythonScriptModule::GetInstance()->WrapQObject(PythonScript::self()->worldstream.get());
}

PyObject* SendRexPrimData(PyObject *self, PyObject *args)
{
    std::cout << "Sending rexprimdata" << std::endl;

    /*rexviewer_EntityObject* py_ent;
    if(!PyArg_ParseTuple(args, "O!", rexviewer_EntityType, &py_ent))
    {
        return NULL;   
    }*/

    unsigned int ent_id_int;
    entity_id_t ent_id;

    if(!PyArg_ParseTuple(args, "I", &ent_id_int))
    {
        PyErr_SetString(PyExc_ValueError, "Getting an entity failed, param should be an integer.");
        return NULL;   
    }

    ent_id = (entity_id_t) ent_id_int;

    RexLogic::RexLogicModule *rexlogic = PythonScript::self()->GetFramework()->GetModule<RexLogic::RexLogicModule>();
    if (rexlogic)
        rexlogic->SendRexPrimData(ent_id); //py_ent->ent_id);

    Py_RETURN_NONE;
}

PyObject* GetTrashFolderId(PyObject* self, PyObject* args)
{
    ProtocolUtilities::InventoryFolderSkeleton *folder = PythonScript::self()->inventory->GetFirstChildFolderByName("Trash");
    if (folder)
        return Py_BuildValue("s", folder->id.ToString().c_str());
    return NULL;
}

PyObject* NetworkUpdate(PyObject *self, PyObject *args)
{   
    //PythonScript::self()->LogDebug("NetworkUpdate");
    unsigned int ent_id_int;
    entity_id_t ent_id;

    if(!PyArg_ParseTuple(args, "I", &ent_id_int))
    {
        PyErr_SetString(PyExc_ValueError, "Getting an entity failed, param should be an integer.");
        return NULL;   
    }

    ent_id = (entity_id_t) ent_id_int;
    
    PythonScriptModule *owner = PythonScriptModule::GetInstance();
    Scene::ScenePtr scene = owner->GetScenePtr();
    if (!scene)
    {
        PyErr_SetString(PyExc_RuntimeError, "default scene not there when trying to use an entity.");
        return NULL;
    }

    Scene::EntityPtr entity = scene->GetEntity(ent_id);
    Scene::Events::SceneEventData event_data(ent_id);
    event_data.entity_ptr_list.push_back(entity);
    PythonScript::self()->GetFramework()->GetEventManager()->SendEvent(owner->scene_event_category_, Scene::Events::EVENT_ENTITY_UPDATED, &event_data);
    
    Py_RETURN_NONE;
}

//XXX \todo make Login a QObject and the other login methods slots so they are all exposed
PyObject* StartLoginOpensim(PyObject *self, PyObject *args)
{
    const char* firstAndLast;
    const char* password;
    const char* serverAddressWithPort;

    if(!PyArg_ParseTuple(args, "sss", &firstAndLast, &password, &serverAddressWithPort))
    {
        PyErr_SetString(PyExc_ValueError, "Opensim login requires three params: User Name, password, server:port");
        return NULL;
    }

    QMap<QString, QString> map;
    map["AvatarType"] = "OpenSim";
    map["Username"] = firstAndLast;
    map["Password"] = password;
    map["WorldAddress"] = serverAddressWithPort;

    Foundation::LoginServiceInterface *login_service = PythonScript::self()->GetFramework()->GetService<Foundation::LoginServiceInterface>();
    if (login_service)
        login_service->ProcessLoginData(map);

    Py_RETURN_NONE;
}

PyObject *Exit(PyObject *self, PyObject *null)
{
    PythonScript::self()->GetFramework()->Exit();
    Py_RETURN_NONE;
}

PyObject* Logout(PyObject *self)
{
    Foundation::LoginServiceInterface *login_service = PythonScript::self()->GetFramework()->GetService<Foundation::LoginServiceInterface>();
    if (login_service)
        login_service->Logout();

    Py_RETURN_NONE;
}

/*
PyObject* PyEventCallback(PyObject *self, PyObject *args){
    std::cout << "PyEventCallback" << std::endl;
    const char* key;
    const char* message;
    if(!PyArg_ParseTuple(args, "ss", &key, &message))
        Py_RETURN_FALSE; //XXX should raise an exception but that'd require refactor in the comms py backend
    std::cout << key << std::endl;
    std::cout << message << std::endl;
    std::string k(key);
    std::string m(message);
    PythonScript::PythonScriptModule::engineAccess->NotifyScriptEvent(k, m);
    Py_RETURN_TRUE;
}
*/

// XXX NOTE: there apparently is a way to expose bound c++ methods? 
// http://mail.python.org/pipermail/python-list/2004-September/282436.html
static PyMethodDef EmbMethods[] = {
    {"sendChat", (PyCFunction)SendChat, METH_VARARGS,
    "Send the given text as an in-world chat message."},

    {"getQWorldBuildingHandler", (PyCFunction)GetQWorldBuildingHandler, METH_NOARGS,
    "Get the World Building Modules python handler as a QObject"},

    {"getEntityByUUID", (PyCFunction)GetEntityByUUID, METH_VARARGS,
    "Gets the entity with the given UUID."},

    {"removeEntity", (PyCFunction)RemoveEntity, METH_VARARGS,
    "Creates a new entity with the given ID, and returns it."},

    {"getCameraYawPitch", (PyCFunction)GetCameraYawPitch, METH_VARARGS,
    "Returns the camera yaw and pitch."},

    {"setCameraYawPitch", (PyCFunction)SetCameraYawPitch, METH_VARARGS,
    "Sets the camera yaw and pitch."},

    {"setAvatarYaw", (PyCFunction)SetAvatarYaw, METH_VARARGS,
    "Changes the avatar yaw with the given amount. Keys left/right are -1/+1."},    

    {"rayCast", (PyCFunction)RayCast, METH_VARARGS,
    "RayCasting from camera to point (x,y)."},

    {"switchCameraState", (PyCFunction)SwitchCameraState, METH_VARARGS,
    "Switching the camera mode from free to thirdperson and back again."},

    {"setAvatarRotation", (PyCFunction)SetAvatarRotation, METH_VARARGS,
    "Rotating the avatar."},

    {"sendEvent", (PyCFunction)SendEvent, METH_VARARGS,
    "Send an event id (WIP other stuff)."},

    {"takeScreenshot", (PyCFunction)TakeScreenshot, METH_VARARGS,
    "Takes a screenshot and saves it to a timestamped file."},

    {"logInfo", (PyCFunction)PyLogInfo, METH_VARARGS,
    "Prints a text using the LogInfo-method."},

    {"logDebug", (PyCFunction)PyLogDebug, METH_VARARGS,
    "Prints a debug text using the LogDebug-method."},

    {"logWarning", (PyCFunction)PyLogWarning, METH_VARARGS,
    "Prints a text using the LogWarning-method."},

    {"logError", (PyCFunction)PyLogError, METH_VARARGS,
    "Prints a text using the LogError-method."},

    {"getUiView", (PyCFunction)GetUIView, METH_NOARGS, 
    "Gets the Naali-Qt UI main view"},
    
    {"disconnectUiViewSignals", (PyCFunction)DisconnectUIViewSignals, METH_NOARGS,
    "Disconnects all signals from uiview (temporary HACK)"},

    {"sendRexPrimData", (PyCFunction)SendRexPrimData, METH_VARARGS,
    "updates prim data to the server - now for applying a mesh to an object"},

    {"networkUpdate", (PyCFunction)NetworkUpdate, METH_VARARGS, 
    "Does a network update for the Scene."},

    {"startLoginOpensim", (PyCFunction)StartLoginOpensim, METH_VARARGS,
    "Starts login using OpenSim authentication: expects User Name, password, server:port"},

    {"logout", (PyCFunction)Logout, METH_NOARGS,
    "Log out from the world. Made for test script to be able to stop."},

    {"exit", (PyCFunction)Exit, METH_NOARGS,
    "Exits viewer. Takes no arguments."},

    {"getRexLogic", (PyCFunction)GetRexLogic, METH_NOARGS,
    "Gets the RexLogicModule."},

    {"getServerConnection", (PyCFunction)GetServerConnection, METH_NOARGS,
    "Gets the server connection."},    

    {"getTrashFolderId", (PyCFunction)GetTrashFolderId, METH_VARARGS, 
    "gets the trash folder id"},

    {"createUiProxyWidget", (PyCFunction)CreateUiProxyWidget, METH_VARARGS, 
    "creates a new UiProxyWidget"},

//    {"getEntityMatindicesWithTexture", (PyCFunction)GetEntityMatindicesWithTexture, METH_VARARGS, 
//    "Finds all entities with material indices which are using the given texture"},

    {"checkSceneForTexture", (PyCFunction)CheckSceneForTexture, METH_VARARGS, 
    "Return true if texture exists in scene, otherwise false: Parameters: textureuuid"},

    {"applyUICanvasToSubmeshes", (PyCFunction)ApplyUICanvasToSubmeshes, METH_VARARGS, 
    "Applies a ui canvas to the given submeshes of the entity. Parameters: entity id, list of submeshes (material indices), uicanvas (internal mode required)"},
    
    {"getSubmeshesWithTexture", (PyCFunction)GetSubmeshesWithTexture, METH_VARARGS, 
    "Find the submeshes in this entity that use the given texture, if any. Parameters: entity id, texture uuid"},
    
    {"getApplicationDataDirectory", (PyCFunction)GetApplicationDataDirectory, METH_NOARGS,
    "Get application data directory."},

    {NULL, NULL, 0, NULL}
};

namespace PythonScript
{
    // virtual
    void PythonScriptModule::Initialize()
    {
        if (!engine_)
            engine_ = PythonScript::PythonEnginePtr(new PythonScript::PythonEngine(framework_));

        engine_->Initialize();

        framework_->GetServiceManager()->RegisterService(Foundation::Service::ST_PythonScripting, engine_);

        assert(!pythonScriptModuleInstance_);
        pythonScriptModuleInstance_ = this;

        apiModule = Py_InitModule("rexviewer", EmbMethods);
        assert(apiModule);
        if (!apiModule)
            return;

        //event constants are now put in PostInit so that the other modules have registered theirs already.
        //XXX what about new event types defined in py-written modules?

        //init PythonQt, implemented in RexPythonQt.cpp
        if (!pythonqt_inited)
        {
            PythonScript::initRexQtPy(apiModule);
            PythonQtObjectPtr mainModule = PythonQt::self()->getMainModule();

            mainModule.addObject("_pythonscriptmodule", this);
            PythonQt::self()->registerClass(&OgreRenderer::Renderer::staticMetaObject);
            PythonQt::self()->registerClass(&Foundation::WorldLogicInterface::staticMetaObject);
            PythonQt::self()->registerClass(&Scene::SceneManager::staticMetaObject);
            PythonQt::self()->registerClass(&MediaPlayer::ServiceInterface::staticMetaObject);
            PythonQt::self()->registerClass(&PythonQtScriptingConsole::staticMetaObject);

            mainModule.addObject("_naali", GetFramework());
            PythonQt::self()->registerClass(&Frame::staticMetaObject);
            PythonQt::self()->registerClass(&DelayedSignal::staticMetaObject);
            PythonQt::self()->registerClass(&ScriptConsole::staticMetaObject);
            PythonQt::self()->registerClass(&Command::staticMetaObject);
            PythonQt::self()->registerClass(&Scene::Entity::staticMetaObject);
            PythonQt::self()->registerClass(&EntityAction::staticMetaObject);

            PythonQt::self()->registerClass(&UiServiceInterface::staticMetaObject);
//            PythonQt::self()->registerClass(&UiProxyWidget::staticMetaObject);
            PythonQt::self()->registerClass(&ISoundService::staticMetaObject);
            PythonQt::self()->registerClass(&Input::staticMetaObject);

            //add placeable and friends when PyEntity goes?
            PythonQt::self()->registerClass(&EC_OgreCamera::staticMetaObject);
            PythonQt::self()->registerClass(&EC_Mesh::staticMetaObject);
            PythonQt::self()->registerClass(&RexLogic::EC_AttachedSound::staticMetaObject);
            PythonQt::self()->registerClass(&AttributeChange::staticMetaObject);
            PythonQt::self()->registerClass(&KeyEvent::staticMetaObject);
            PythonQt::self()->registerClass(&MouseEvent::staticMetaObject);
            PythonQt::self()->registerClass(&InputContext::staticMetaObject);
            PythonQt::self()->registerClass(&EC_Ruler::staticMetaObject);

            pythonqt_inited = true;

            //PythonQt::self()->registerCPPClass("Vector3df", "","", PythonQtCreateObject<Vector3Wrapper>);
            //PythonQt::self()->registerClass(&Vector3::staticMetaObject);
        }

        //load the py written module manager using the py c api directly
        pmmModule = PyImport_ImportModule("modulemanager");
        if (pmmModule == NULL) {
            LogError("Failed to import py modulemanager");
            return;
        }
        pmmDict = PyModule_GetDict(pmmModule);
        if (pmmDict == NULL) {
            LogError("Unable to get modulemanager module namespace");
            return;
        }
        pmmClass = PyDict_GetItemString(pmmDict, "ModuleManager");
        if(pmmClass == NULL) {
            LogError("Unable get ModuleManager class from modulemanager namespace");
            return;
        }
        //instanciating the manager moved to PostInitialize, 
        //'cause it does autoload.py where TestComponent expects the event constants to be there already

        //std::string error;
        //modulemanager = engine_->LoadScript("modulemanager", error); //the pymodule loader & event manager
        //modulemanager = modulemanager->GetObject("ModuleManager"); //instanciates

        LogInfo(Name() + " initialized succesfully.");
    }
}
