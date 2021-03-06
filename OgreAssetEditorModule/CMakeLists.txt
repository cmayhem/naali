# Define target name and output directory
init_target (OgreAssetEditorModule OUTPUT modules/core)

# Define source files
file (GLOB CPP_FILES *.cpp)
file (GLOB H_FILES *.h)
file (GLOB XML_FILES *.xml)
file (GLOB MOC_FILES OgreAssetEditorModule.h EditorManager.h OgreScriptEditor.h PropertyTableWidget.h
    OgreMaterialProperties.h MaterialWizard.h TexturePreviewEditor.h AudioPreviewEditor.h AudioSignalLabel.h
    MeshPreviewEditor.h)
file (GLOB RESOURCE_FILES resource/*.qrc)
set (SOURCE_FILES ${CPP_FILES} ${H_FILES})

add_definitions (-DASSET_EDITOR_MODULE_EXPORTS)

# Qt4 Moc files to subgroup "CMake Moc"
MocFolder ()

# Qt4 Wrap
QT4_WRAP_CPP(MOC_SRCS ${MOC_FILES})
QT4_WRAP_UI(UI_SRCS ${UI_FILES})
QT4_ADD_RESOURCES(RESOURCE_SRCS ${RESOURCE_FILES})

set (FILES_TO_TRANSLATE ${FILES_TO_TRANSLATE} ${H_FILES} ${CPP_FILES} PARENT_SCOPE)

use_package (BOOST)
use_package (POCO)
use_package (OGRE)
use_package (QT4)
use_modules (Core Foundation Interfaces RexCommon ProtocolUtilities AssetModule OgreRenderingModule Input)

build_library (${TARGET_NAME} SHARED ${SOURCE_FILES} ${MOC_SRCS} ${RESOURCE_SRCS})

link_modules (Core Foundation Interfaces RexCommon OgreRenderingModule Interfaces Input)
link_package (BOOST)
link_package (POCO)
link_ogre()
link_package (QT4)

SetupCompileFlagsWithPCH()
CopyModuleXMLFile()

final_target ()
