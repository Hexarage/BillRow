auto_discover_headers(${CMAKE_CURRENT_SOURCE_DIR} PROJECT_HEADERS)

add_library(${PROJECT_NAME}_headers INTERFACE)

target_include_directories(${PROJECT_NAME}_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

#To make the headers show up in IDE
target_sources(${PROJECT_NAME}_headers INTERFACE ${PROJECT_HEADERS})
set_source_files_properties(${PROJECT_HEADERS} PROPERTIES HEADER_FILE_ONLY TRUE)