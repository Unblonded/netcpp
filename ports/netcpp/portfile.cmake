vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Unblonded/netcpp
    REF "v${VERSION}"
    SHA512 07694dc87498d59e1edcc13c1f8a1eb98077d05bd9a4c0d85f60ffbe8179058bde431cd546ad0eb4e9853149145beda57c7eb3438521d0ad5372adf28073f039
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DNETCPP_BUILD_TESTS=OFF
        -DNETCPP_BUILD_EXAMPLES=OFF
        -DNETCPP_INSTALL=ON
        -DNETCPP_FETCH_CURL=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/netcpp)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
