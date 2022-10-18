project(test)

add_executable(test1_basic test1_basic.cpp)

target_link_libraries(test1_basic PRIVATE lips)

add_test(NAME test1_basic COMMAND test1_basic)
