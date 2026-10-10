set(FRICTION_PLUGIN_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/Parameters.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Presets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PluginProcessor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PluginEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/FrictionLookAndFeel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/FrictionMap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/SidePanels.cpp)

# Plugin sources shared by the plugin and the plugin-level test harness
add_library(FrictionPluginSources INTERFACE)
target_sources(FrictionPluginSources INTERFACE ${FRICTION_PLUGIN_SOURCES})
target_include_directories(FrictionPluginSources INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(FrictionPluginSources INTERFACE
    FrictionCore
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_gui_basics)
target_compile_definitions(FrictionPluginSources INTERFACE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0)

juce_add_plugin(Friction
    PRODUCT_NAME "Friction"
    COMPANY_NAME "Friction"
    BUNDLE_ID "com.friction.roughness"
    PLUGIN_MANUFACTURER_CODE Frct
    PLUGIN_CODE Frc1
    FORMATS VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Analyzer
    VERSION ${PROJECT_VERSION})

target_link_libraries(Friction
    PRIVATE
        FrictionPluginSources
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(FRICTION_BUILD_TESTS)
    juce_add_console_app(FrictionPluginTests PRODUCT_NAME "FrictionPluginTests")
    target_sources(FrictionPluginTests PRIVATE ${PROJECT_SOURCE_DIR}/tests/plugin/PluginTests.cpp)
    target_include_directories(FrictionPluginTests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(FrictionPluginTests
        PRIVATE
            FrictionPluginSources
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
    add_test(NAME FrictionPluginTests COMMAND FrictionPluginTests --out ${CMAKE_BINARY_DIR}/test_output)
endif()
