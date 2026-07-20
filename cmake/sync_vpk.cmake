if (NOT DEFINED INPUT_VPK OR NOT DEFINED OUTPUT_DIR OR NOT DEFINED OUTPUT_NAME)
  message(FATAL_ERROR "sync_vpk.cmake requires INPUT_VPK, OUTPUT_DIR, and OUTPUT_NAME.")
endif ()

if (NOT EXISTS "${INPUT_VPK}")
  message(FATAL_ERROR "Input VPK does not exist: ${INPUT_VPK}")
endif ()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")

file(GLOB existing_vpks "${OUTPUT_DIR}/*.vpk")
if (existing_vpks)
  file(REMOVE ${existing_vpks})
endif ()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${INPUT_VPK}" "${OUTPUT_DIR}/${OUTPUT_NAME}"
  RESULT_VARIABLE copy_result
)

if (NOT copy_result EQUAL 0)
  message(FATAL_ERROR "Failed to copy canonical VPK to ${OUTPUT_DIR}/${OUTPUT_NAME}")
endif ()
