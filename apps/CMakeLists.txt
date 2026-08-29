add_executable(admission-fabric-coordinator coordinator/main.cpp)
target_link_libraries(admission-fabric-coordinator PRIVATE admission_fabric)

add_executable(admission-fabric-agent agent/main.cpp)
target_link_libraries(admission-fabric-agent PRIVATE admission_fabric)

add_executable(admission-fabricctl ctl/main.cpp)
target_link_libraries(admission-fabricctl PRIVATE admission_fabric)
