vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Deutsches-Klimarechenzentrum/libaec
    REF v1.1.7
    SHA512 55bd605590015e0f903a231268265051336c172c18935fdfecead5630454a99811f3f196117556ad1b62f3052d54894fd8e257a9ea9f45cc03f59c93cde00cda
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_TESTING=OFF
)

vcpkg_cmake_install()

# libaec's own generated libaec-config.cmake unconditionally include()s both
# a static and a shared "*-targets.cmake" file, regardless of which was
# actually built. These triplets are static-only, so the shared one never
# gets generated - which breaks anything (like HDF5) that does
# find_package(libaec CONFIG). Stubbing it out as an empty file lets that
# include() succeed harmlessly; nothing in a static build needs it anyway.
foreach(CFG_DIR "lib/cmake/libaec" "debug/lib/cmake/libaec")
    if(EXISTS "${CURRENT_PACKAGES_DIR}/${CFG_DIR}/libaec-config.cmake")
        file(WRITE "${CURRENT_PACKAGES_DIR}/${CFG_DIR}/libaec_shared-targets.cmake" "")
    endif()
endforeach()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_copy_pdbs()

file(INSTALL "${SOURCE_PATH}/LICENSE.txt" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
