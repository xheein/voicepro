
cmake_minimum_required(VERSION 3.20)


project(VoicePro VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_CXX_STANDARD_REQUIRED ON)


if(WIN32)
    add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX)
endif()


set(SOURCES
    src/main.cpp                 # 程序入口
    src/audio_engine.cpp          # 音频引擎核心逻辑
    src/audio_session.cpp         # 音频会话管理
    src/process_loopback.cpp      # 进程音频采集（回环采集）
    src/microphone_capture.cpp    # 麦克风音频采集
    src/audio_renderer.cpp        # 音频渲染（播放）
    src/gui.cpp                   # 图形用户界面实现
    src/language.cpp              # 语言管理与国际化
    src/resources.rc              # Windows 资源文件
)


set(HEADERS
    src/audio_engine.h
    src/audio_session.h
    src/process_loopback.h
    src/microphone_capture.h
    src/audio_renderer.h
    src/gui.h
    src/language.h                # 语言管理头文件
    src/ring_buffer.h            
)

add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${HEADERS})


target_link_libraries(${PROJECT_NAME} PRIVATE
    ole32
    oleaut32
    uuid
    winmm
    comctl32
    uxtheme
)


if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /utf-8)
    
    set_target_properties(${PROJECT_NAME} PROPERTIES
        VS_DPI_AWARE "PerMonitor"
    )
endif()
