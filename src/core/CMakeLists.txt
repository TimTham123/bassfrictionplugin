add_library(FrictionCore STATIC
    Advisor.cpp
    BellBank.cpp
    FrictionEngine.cpp
    LowBandTracker.cpp
    OnsetDetector.cpp
    PartialShifter.cpp
    RealFFT.cpp
    Roughness.cpp
    SpectralAnalyzer.cpp
    SynthBank.cpp
    WholeShifter.cpp)

target_include_directories(FrictionCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(FrictionCore PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(FrictionCore PRIVATE /W4 /wd4244 /wd4267 /wd4305)
    target_compile_definitions(FrictionCore PUBLIC _USE_MATH_DEFINES NOMINMAX)
else()
    target_compile_options(FrictionCore PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
