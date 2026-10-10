add_executable(FrictionTests
    TestMain.cpp
    TestsAnalysis.cpp
    TestsSmooth.cpp
    TestsAdvisor.cpp
    TestsRetune.cpp
    TestsTension.cpp
    TestsRobustness.cpp
    TestsDemos.cpp
    TestsPerformance.cpp)

target_link_libraries(FrictionTests PRIVATE FrictionCore)
target_compile_features(FrictionTests PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(FrictionTests PRIVATE /W3 /bigobj)
else()
    target_compile_options(FrictionTests PRIVATE -Wall -Wno-unused-parameter -Wno-unused-variable)
endif()

add_test(NAME FrictionTests
         COMMAND FrictionTests --out ${CMAKE_BINARY_DIR}/test_output --renders ${CMAKE_BINARY_DIR}/renders
                               --report ${CMAKE_BINARY_DIR}/TEST_REPORT.md)
