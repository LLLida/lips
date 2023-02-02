project(test)

function (make_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE lips)
  add_test(NAME ${name} COMMAND ${name})
endfunction ()

make_test(test1_basic)
make_test(test2_gc)
make_test(test3_plist)
