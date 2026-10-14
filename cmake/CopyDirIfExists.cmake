# copy_directory that succeeds when the source doesn't exist — used for
# optional runtime staging (e.g. vcpkg DLLs only exist for shared triplets).
if(EXISTS "${SRC}" AND IS_DIRECTORY "${SRC}")
    file(COPY "${SRC}/" DESTINATION "${DST}")
endif()
