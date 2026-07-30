if(NOT DEFINED FFMPEG_EXECUTABLE)
    message(FATAL_ERROR "FFMPEG_EXECUTABLE is required")
endif()
if(NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "OUTPUT_FILE is required")
endif()

file(REMOVE "${OUTPUT_FILE}")
execute_process(
    COMMAND
        "${FFMPEG_EXECUTABLE}"
        -hide_banner
        -loglevel error
        -y
        -init_hw_device d3d12va=d3d12
        -filter_hw_device d3d12
        -f lavfi
        -i
        "color=c=red:s=160x90:r=4:d=1,drawbox=x=80:y=0:w=80:h=90:color=blue:t=fill"
        -frames:v 4
        -vf "format=p010le,hwupload"
        -c:v hevc_d3d12va
        -profile:v main10
        -color_range tv
        -colorspace bt2020nc
        -color_primaries bt2020
        -color_trc smpte2084
        -g 4
        -bsf:v
        "hevc_metadata=video_full_range_flag=0:colour_primaries=9:transfer_characteristics=16:matrix_coefficients=9"
        "${OUTPUT_FILE}"
    RESULT_VARIABLE generation_result
    ERROR_VARIABLE generation_error
)

if(NOT generation_result EQUAL 0)
    file(REMOVE "${OUTPUT_FILE}")
    string(STRIP "${generation_error}" generation_error)
    message(
        STATUS
        "HEVC Main10 test media generation unavailable; "
        "the capability-gated test will skip: ${generation_error}"
    )
endif()
