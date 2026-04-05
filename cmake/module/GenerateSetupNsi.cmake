# Copyright (c) 2023-present The Lambdanio Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

function(generate_setup_nsi)
  set(abs_top_srcdir ${PROJECT_SOURCE_DIR})
  set(abs_top_builddir ${PROJECT_BINARY_DIR})
  set(CLIENT_URL ${PROJECT_HOMEPAGE_URL})
  set(CLIENT_TARNAME "lambdanio")
  set(LAMBDANIO_WRAPPER_NAME "lambdanio")
  set(LAMBDANIO_GUI_NAME "lambdanio-qt")
  set(LAMBDANIO_DAEMON_NAME "lambdaniod")
  set(LAMBDANIO_CLI_NAME "lambdanio-cli")
  set(LAMBDANIO_TX_NAME "lambdanio-tx")
  set(LAMBDANIO_WALLET_TOOL_NAME "lambdanio-wallet")
  set(LAMBDANIO_TEST_NAME "test_lambdanio")
  set(EXEEXT ${CMAKE_EXECUTABLE_SUFFIX})
  configure_file(${PROJECT_SOURCE_DIR}/share/setup.nsi.in ${PROJECT_BINARY_DIR}/lambdanio-win64-setup.nsi USE_SOURCE_PERMISSIONS @ONLY)
endfunction()
