#For auto discover laziness
auto_discover_sources(${CMAKE_CURRENT_SOURCE_DIR} SOURCES)
auto_discover_headers(${CMAKE_CURRENT_SOURCE_DIR} PRIVATE_HEADERS)

#For proper stuff: explicitly list stuff with set(SOURCES ...)
#and set(PRIVATE_HEADERS ...)


#Target definitions
add_executable(${PROJECT_NAME} ${SOURCES} ${PRIVATE_HEADERS})

#Link against the header interface lib
target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_NAME}_headers)

#Include directories for private headers
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

#Platform specific settings cruft
if(UNIX AND NOT APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE pthread dl)
endif()

#Installation
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)