cmake_minimum_required(VERSION 3.16)
project(NebulaClangdFixture LANGUAGES CXX)
add_library(math_fixture STATIC math.cpp caller.cpp)
set_target_properties(math_fixture PROPERTIES CXX_STANDARD 17)
