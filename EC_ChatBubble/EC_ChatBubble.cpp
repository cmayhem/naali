/**
 *  For conditions of distribution and use, see copyright notice in license.txt
 *
 *  @file   EC_ChatBubble.cpp
 *  @brief  EC_ChatBubble 
 */

#include "StableHeaders.h"
#include "EC_ChatBubble.h"
#include "ModuleInterface.h"
#include "Renderer.h"
#include "EC_OgrePlaceable.h"
#include "Entity.h"
#include "OgreMaterialUtils.h"

#include <Ogre.h>
#include <OgreBillboardSet.h>
#include <OgreTextureManager.h>
#include <OgreResource.h>

EC_ChatBubble::EC_ChatBubble(Foundation::ModuleInterface *module) :
    Foundation::ComponentInterface(module->GetFramework())
{
}

EC_ChatBubble::~EC_ChatBubble()
{
}

void EC_ChatBubble::Show(const Vector3df &offset, float timeToShow, const char *imageName)
{
    boost::shared_ptr<OgreRenderer::Renderer> renderer = framework_->GetServiceManager()->GetService
        <OgreRenderer::Renderer>(Foundation::Service::ST_Renderer).lock();
    if (!renderer)
        return;

    Ogre::SceneManager *scene = renderer->GetSceneManager();
    assert(scene);
    if (!scene)
        return;

    Scene::Entity *entity = GetParentEntity();
    assert(entity);
    if (!entity)
        return;

    OgreRenderer::EC_OgrePlaceable *node = entity->GetComponent<OgreRenderer::EC_OgrePlaceable>().get();
    if (!node)
        return;

    Ogre::SceneNode *ogreNode = node->GetSceneNode();
    assert(ogreNode);
    if (!ogreNode)
        return;

    Ogre::TextureManager &manager = Ogre::TextureManager::getSingleton();
    Ogre::Texture *tex = dynamic_cast<Ogre::Texture *>(manager.getByName(imageName).get());
    if (!tex)
    {
        ///\bug OGRE doesn't seem to add all texture to the resource group although the texture
        ///     exists in folder spesified in the resource.cfg
        std::stringstream ss;
        ss << "Ogre Texture \"" << imageName << "\" not found!";
        std::cout << ss.str() << std::endl;

        Ogre::ResourcePtr rp = manager.create(imageName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        if (!rp.isNull())
        {
            std::cout << "But should be now..." << std::endl;
        }
    }

    ///\todo GetUniqueObjectName generates object names, not material or billboardset names, but anything unique goes.
    /// Perhaps would be nicer to just have a GetUniqueName(string prefix)?

    Ogre::BillboardSet *billboardSet = scene->createBillboardSet(renderer->GetUniqueObjectName(), 1);
    assert(billboardSet);

    std::string newName = std::string("material") + renderer->GetUniqueObjectName(); 
    Ogre::MaterialPtr material = OgreRenderer::CloneMaterial("UnlitTexturedSoftAlpha", newName);
    OgreRenderer::SetTextureUnitOnMaterial(material, imageName);
    billboardSet->setMaterialName(newName);

    Ogre::Billboard *billboard = billboardSet->createBillboard(Ogre::Vector3(offset.x, offset.y, offset.z));
    assert(billboard);
    billboard->setDimensions(2, 1);

    ogreNode->attachObject(billboardSet);
}
