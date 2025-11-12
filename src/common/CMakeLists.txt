add_library(CommonHeaders INTERFACE)
target_include_directories(CommonHeaders INTERFACE include)
target_compile_definitions(CommonHeaders INTERFACE 
    NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_directories(CommonHeaders INTERFACE glm::glm)