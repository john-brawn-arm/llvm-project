add_lldb_library(lldbPluginABIARM PLUGIN
  ABIARM.cpp
  ABIMacOSX_arm.cpp
  ABISysV_arm.cpp

  LINK_LIBS
    lldbCore
    lldbSymbol
    lldbTarget
    lldbPluginProcessUtility
    lldbValueObject
  LINK_COMPONENTS
    Support
    TargetParser
  )
