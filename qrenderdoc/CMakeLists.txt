option(STATIC_QRENDERDOC "Compile qrenderdoc as static" OFF)
option(QRENDERDOC_NO_CXX11_REGEX "Disable C++11 regex in scintilla" OFF)

if( NOT DEFINED QT_QMAKE_EXECUTABLE )
    if( DEFINED QMAKE_EXECUTABLE )
        set( QT_QMAKE_EXECUTABLE "${QMAKE_EXECUTABLE}" )
    else()
        set( QT_QMAKE_EXECUTABLE "qmake" )
    endif()
endif()

set(QMAKE_QT5_COMMAND ${QT_QMAKE_EXECUTABLE} CACHE STRING "Command to run to invoke Qt5's qmake. Normally this is qmake, possibly with qtchooser, but might be qmake-qt5")
set(RENDERDOC_SWIG_PACKAGE https://github.com/baldurk/swig/archive/renderdoc-modified-7.zip CACHE STRING "The location where RenderDoc's swig fork source can be found. By default points to the URL on github but can be pointed to a local file.")
set(RENDERDOC_PCRE_PACKAGE https://sourceforge.net/projects/pcre/files/pcre/8.45/pcre-8.45.tar.gz CACHE STRING "The location where the pcre library package can be found. By default points to the URL on sourceforge but can be pointed to a local file.")

if(WIN32 AND ENABLE_QRENDERDOC)
    # ============================================================
    # Windows build using bundled Qt library
    # ============================================================

    # Qt paths - use the bundled Qt in 3rdparty/qt
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(QT_PLATFORM_DIR "x64")
    else()
        set(QT_PLATFORM_DIR "Win32")
    endif()

    set(QT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/qt/${QT_PLATFORM_DIR}")
    set(QT_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/qt/include")
    set(QT_BIN_DIR "${QT_ROOT}/bin")
    set(QT_LIB_DIR "${QT_ROOT}/lib")

    set(QT_MOC "${QT_BIN_DIR}/moc.exe")
    set(QT_UIC "${QT_BIN_DIR}/uic.exe")
    set(QT_RCC "${QT_BIN_DIR}/rcc.exe")

    set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

    file(MAKE_DIRECTORY "${GENERATED_DIR}")

    # All source files
    set(qrenderdoc_sources
        # Code
        Code/BufferFormatter.cpp
        Code/CaptureContext.cpp
        Code/MiniQtHelper.cpp
        Code/precompiled.cpp
        Code/qprocessinfo.cpp
        Code/QRDUtils.cpp
        Code/qrenderdoc.cpp
        Code/ReplayManager.cpp
        Code/Resources.cpp
        Code/RGPInterop.cpp
        Code/ScintillaSyntax.cpp
        Code/Interface/Analytics.cpp
        Code/Interface/PersistantConfig.cpp
        Code/Interface/QRDInterface.cpp
        Code/Interface/RemoteHost.cpp
        Code/Interface/ShaderProcessingTool.cpp
        Code/pyrenderdoc/PythonContext.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/generated/swig_stubs.cpp"

        # Styles
        Styles/RDStyle/RDStyle.cpp
        Styles/RDTweakedNativeStyle/RDTweakedNativeStyle.cpp
        Styles/StyleData.cpp

        # Widgets
        Widgets/AnnotationDisplay.cpp
        Widgets/BufferFormatSpecifier.cpp
        Widgets/CollapseGroupBox.cpp
        Widgets/ComputeDebugSelector.cpp
        Widgets/CustomPaintWidget.cpp
        Widgets/FindReplace.cpp
        Widgets/MarkerBreadcrumbs.cpp
        Widgets/OrderedListEditor.cpp
        Widgets/PipelineFlowChart.cpp
        Widgets/RangeHistogram.cpp
        Widgets/ReplayOptionsSelector.cpp
        Widgets/ResourcePreview.cpp
        Widgets/TextureGoto.cpp
        Widgets/ThumbnailStrip.cpp
        Widgets/Extended/RDDoubleSpinBox.cpp
        Widgets/Extended/RDHeaderView.cpp
        Widgets/Extended/RDLabel.cpp
        Widgets/Extended/RDLineEdit.cpp
        Widgets/Extended/RDListView.cpp
        Widgets/Extended/RDListWidget.cpp
        Widgets/Extended/RDMenu.cpp
        Widgets/Extended/RDSplitter.cpp
        Widgets/Extended/RDTableView.cpp
        Widgets/Extended/RDTableWidget.cpp
        Widgets/Extended/RDTextEdit.cpp
        Widgets/Extended/RDToolButton.cpp
        Widgets/Extended/RDTreeView.cpp
        Widgets/Extended/RDTreeWidget.cpp

        # Windows
        Windows/APIInspector.cpp
        Windows/BufferViewer.cpp
        Windows/CommentView.cpp
        Windows/DebugMessageView.cpp
        Windows/DescriptorViewer.cpp
        Windows/EventBrowser.cpp
        Windows/LogView.cpp
        Windows/MainWindow.cpp
        Windows/PerformanceCounterViewer.cpp
        Windows/RayTraceInfoViewer.cpp
        Windows/RayTraceInfoViewer.h
        Windows/PixelHistoryView.cpp
        Windows/PythonShell.cpp
        Windows/ResourceInspector.cpp
        Windows/ShaderMessageViewer.cpp
        Windows/ShaderViewer.cpp
        Windows/StatisticsViewer.cpp
        Windows/TextureViewer.cpp
        Windows/TimelineBar.cpp
        Windows/Dialogs/AboutDialog.cpp
        Windows/Dialogs/AnalyticsConfirmDialog.cpp
        Windows/Dialogs/AnalyticsPromptDialog.cpp
        Windows/Dialogs/AxisMappingDialog.cpp
        Windows/Dialogs/CameraControlsDialog.cpp
        Windows/Dialogs/CaptureDialog.cpp
        Windows/Dialogs/ConfigEditor.cpp
        Windows/Dialogs/CrashDialog.cpp
        Windows/Dialogs/EnvironmentEditor.cpp
        Windows/Dialogs/ExtensionManager.cpp
        Windows/Dialogs/LiveCapture.cpp
        Windows/Dialogs/PerformanceCounterSelection.cpp
        Windows/Dialogs/ProjectionGuessDialog.cpp
        Windows/Dialogs/RemoteManager.cpp
        Windows/Dialogs/SettingsDialog.cpp
        Windows/Dialogs/SuggestRemoteDialog.cpp
        Windows/Dialogs/TextureSaveDialog.cpp
        Windows/Dialogs/TipsDialog.cpp
        Windows/Dialogs/UpdateDialog.cpp
        Windows/Dialogs/VirtualFileDialog.cpp
        Windows/PipelineState/D3D11PipelineStateViewer.cpp
        Windows/PipelineState/D3D12PipelineStateViewer.cpp
        Windows/PipelineState/GLPipelineStateViewer.cpp
        Windows/PipelineState/PipelineStateViewer.cpp
        Windows/PipelineState/VulkanPipelineStateViewer.cpp

        # 3rdparty
        3rdparty/flowlayout/FlowLayout.cpp
        3rdparty/toolwindowmanager/ToolWindowManager.cpp
        3rdparty/toolwindowmanager/ToolWindowManagerArea.cpp
        3rdparty/toolwindowmanager/ToolWindowManagerSplitter.cpp
        3rdparty/toolwindowmanager/ToolWindowManagerTabBar.cpp
        3rdparty/toolwindowmanager/ToolWindowManagerWrapper.cpp
    )

    # Scintilla sources (compiled without PCH)
    set(scintilla_sources
        3rdparty/scintilla/src/AutoComplete.cxx
        3rdparty/scintilla/src/CallTip.cxx
        3rdparty/scintilla/src/CaseConvert.cxx
        3rdparty/scintilla/src/CaseFolder.cxx
        3rdparty/scintilla/src/Catalogue.cxx
        3rdparty/scintilla/src/CellBuffer.cxx
        3rdparty/scintilla/src/CharClassify.cxx
        3rdparty/scintilla/src/ContractionState.cxx
        3rdparty/scintilla/src/Decoration.cxx
        3rdparty/scintilla/src/Document.cxx
        3rdparty/scintilla/src/EditModel.cxx
        3rdparty/scintilla/src/Editor.cxx
        3rdparty/scintilla/src/EditView.cxx
        3rdparty/scintilla/src/ExternalLexer.cxx
        3rdparty/scintilla/src/Indicator.cxx
        3rdparty/scintilla/src/KeyMap.cxx
        3rdparty/scintilla/src/LineMarker.cxx
        3rdparty/scintilla/src/MarginView.cxx
        3rdparty/scintilla/src/PerLine.cxx
        3rdparty/scintilla/src/PositionCache.cxx
        3rdparty/scintilla/src/RESearch.cxx
        3rdparty/scintilla/src/RunStyles.cxx
        3rdparty/scintilla/src/ScintillaBase.cxx
        3rdparty/scintilla/src/Selection.cxx
        3rdparty/scintilla/src/Style.cxx
        3rdparty/scintilla/src/UniConversion.cxx
        3rdparty/scintilla/src/ViewStyle.cxx
        3rdparty/scintilla/src/XPM.cxx
        3rdparty/scintilla/lexers/LexCPP.cxx
        3rdparty/scintilla/lexers/LexDiff.cxx
        3rdparty/scintilla/lexers/LexErrorList.cxx
        3rdparty/scintilla/lexers/LexHTML.cxx
        3rdparty/scintilla/lexers/LexJSON.cxx
        3rdparty/scintilla/lexers/LexNull.cxx
        3rdparty/scintilla/lexers/LexPython.cxx
        3rdparty/scintilla/lexers/LexRust.cxx
        3rdparty/scintilla/lexlib/Accessor.cxx
        3rdparty/scintilla/lexlib/CharacterCategory.cxx
        3rdparty/scintilla/lexlib/CharacterSet.cxx
        3rdparty/scintilla/lexlib/LexerBase.cxx
        3rdparty/scintilla/lexlib/LexerModule.cxx
        3rdparty/scintilla/lexlib/LexerNoExceptions.cxx
        3rdparty/scintilla/lexlib/LexerSimple.cxx
        3rdparty/scintilla/lexlib/PropSetSimple.cxx
        3rdparty/scintilla/lexlib/StyleContext.cxx
        3rdparty/scintilla/lexlib/WordList.cxx
        3rdparty/scintilla/qt/ScintillaEditBase/PlatQt.cpp
        3rdparty/scintilla/qt/ScintillaEditBase/ScintillaEditBase.cpp
        3rdparty/scintilla/qt/ScintillaEditBase/ScintillaQt.cpp
        3rdparty/scintilla/qt/ScintillaEdit/ScintillaDocument.cpp
        3rdparty/scintilla/qt/ScintillaEdit/ScintillaEdit.cpp
    )

    # UI files for UIC (generate ui_*.h)
    set(ui_files
        Widgets/BufferFormatSpecifier.ui
        Widgets/ComputeDebugSelector.ui
        Widgets/FindReplace.ui
        Widgets/ReplayOptionsSelector.ui
        Widgets/ResourcePreview.ui
        Widgets/ThumbnailStrip.ui
        Windows/APIInspector.ui
        Windows/BufferViewer.ui
        Windows/CommentView.ui
        Windows/DebugMessageView.ui
        Windows/DescriptorViewer.ui
        Windows/Dialogs/AboutDialog.ui
        Windows/Dialogs/AnalyticsConfirmDialog.ui
        Windows/Dialogs/AnalyticsPromptDialog.ui
        Windows/Dialogs/AxisMappingDialog.ui
        Windows/Dialogs/CameraControlsDialog.ui
        Windows/Dialogs/CaptureDialog.ui
        Windows/Dialogs/ConfigEditor.ui
        Windows/Dialogs/CrashDialog.ui
        Windows/Dialogs/EnvironmentEditor.ui
        Windows/Dialogs/ExtensionManager.ui
        Windows/Dialogs/LiveCapture.ui
        Windows/Dialogs/PerformanceCounterSelection.ui
        Windows/Dialogs/ProjectionGuessDialog.ui
        Windows/Dialogs/RemoteManager.ui
        Windows/Dialogs/SettingsDialog.ui
        Windows/Dialogs/SuggestRemoteDialog.ui
        Windows/Dialogs/TextureSaveDialog.ui
        Windows/Dialogs/TipsDialog.ui
        Windows/Dialogs/UpdateDialog.ui
        Windows/Dialogs/VirtualFileDialog.ui
        Windows/EventBrowser.ui
        Windows/LogView.ui
        Windows/MainWindow.ui
        Windows/PerformanceCounterViewer.ui
        Windows/RayTraceInfoViewer.ui
        Windows/PipelineState/D3D11PipelineStateViewer.ui
        Windows/PipelineState/D3D12PipelineStateViewer.ui
        Windows/PipelineState/GLPipelineStateViewer.ui
        Windows/PipelineState/PipelineStateViewer.ui
        Windows/PipelineState/VulkanPipelineStateViewer.ui
        Windows/PixelHistoryView.ui
        Windows/PythonShell.ui
        Windows/ResourceInspector.ui
        Windows/ShaderMessageViewer.ui
        Windows/ShaderViewer.ui
        Windows/StatisticsViewer.ui
        Windows/TextureViewer.ui
    )

    # Headers that need MOC processing
    set(moc_headers
        Code/QRDUtils.h
        Code/pyrenderdoc/PythonContext.h
        Styles/RDStyle/RDStyle.h
        Styles/RDTweakedNativeStyle/RDTweakedNativeStyle.h
        Widgets/AnnotationDisplay.h
        Widgets/BufferFormatSpecifier.h
        Widgets/CollapseGroupBox.h
        Widgets/ComputeDebugSelector.h
        Widgets/CustomPaintWidget.h
        Widgets/FindReplace.h
        Widgets/MarkerBreadcrumbs.h
        Widgets/OrderedListEditor.h
        Widgets/PipelineFlowChart.h
        Widgets/RangeHistogram.h
        Widgets/ReplayOptionsSelector.h
        Widgets/ResourcePreview.h
        Widgets/TextureGoto.h
        Widgets/ThumbnailStrip.h
        Widgets/Extended/RDDoubleSpinBox.h
        Widgets/Extended/RDHeaderView.h
        Widgets/Extended/RDLabel.h
        Widgets/Extended/RDLineEdit.h
        Widgets/Extended/RDListView.h
        Widgets/Extended/RDListWidget.h
        Widgets/Extended/RDMenu.h
        Widgets/Extended/RDSplitter.h
        Widgets/Extended/RDTableView.h
        Widgets/Extended/RDTableWidget.h
        Widgets/Extended/RDTextEdit.h
        Widgets/Extended/RDToolButton.h
        Widgets/Extended/RDTreeView.h
        Widgets/Extended/RDTreeWidget.h
        Windows/APIInspector.h
        Windows/BufferViewer.h
        Windows/CommentView.h
        Windows/DebugMessageView.h
        Windows/DescriptorViewer.h
        Windows/Dialogs/AboutDialog.h
        Windows/Dialogs/AnalyticsConfirmDialog.h
        Windows/Dialogs/AnalyticsPromptDialog.h
        Windows/Dialogs/AxisMappingDialog.h
        Windows/Dialogs/CameraControlsDialog.h
        Windows/Dialogs/CaptureDialog.h
        Windows/Dialogs/ConfigEditor.h
        Windows/Dialogs/CrashDialog.h
        Windows/Dialogs/EnvironmentEditor.h
        Windows/Dialogs/ExtensionManager.h
        Windows/Dialogs/LiveCapture.h
        Windows/Dialogs/PerformanceCounterSelection.h
        Windows/Dialogs/ProjectionGuessDialog.h
        Windows/Dialogs/RemoteManager.h
        Windows/Dialogs/SettingsDialog.h
        Windows/Dialogs/SuggestRemoteDialog.h
        Windows/Dialogs/TextureSaveDialog.h
        Windows/Dialogs/TipsDialog.h
        Windows/Dialogs/UpdateDialog.h
        Windows/Dialogs/VirtualFileDialog.h
        Windows/EventBrowser.h
        Windows/LogView.h
        Windows/MainWindow.h
        Windows/PerformanceCounterViewer.h
        Windows/RayTraceInfoViewer.h
        Windows/PipelineState/D3D11PipelineStateViewer.h
        Windows/PipelineState/D3D12PipelineStateViewer.h
        Windows/PipelineState/GLPipelineStateViewer.h
        Windows/PipelineState/PipelineStateViewer.h
        Windows/PipelineState/VulkanPipelineStateViewer.h
        Windows/PixelHistoryView.h
        Windows/PythonShell.h
        Windows/ResourceInspector.h
        Windows/ShaderMessageViewer.h
        Windows/ShaderViewer.h
        Windows/StatisticsViewer.h
        Windows/TextureViewer.h
        Windows/TimelineBar.h

        # 3rdparty headers with Q_OBJECT
        3rdparty/scintilla/qt/ScintillaEditBase/ScintillaEditBase.h
        3rdparty/scintilla/qt/ScintillaEditBase/ScintillaQt.h
        3rdparty/scintilla/qt/ScintillaEdit/ScintillaDocument.h
        3rdparty/scintilla/qt/ScintillaEdit/ScintillaEdit.h
        3rdparty/toolwindowmanager/ToolWindowManager.h
        3rdparty/toolwindowmanager/ToolWindowManagerArea.h
        3rdparty/toolwindowmanager/ToolWindowManagerSplitter.h
        3rdparty/toolwindowmanager/ToolWindowManagerTabBar.h
        3rdparty/toolwindowmanager/ToolWindowManagerWrapper.h
    )

    # QRC files
    set(qrc_files
        Resources/resources.qrc
        Resources/qtconf.qrc
    )

    # ----- UIC: Generate ui_*.h from .ui files -----
    set(generated_ui_headers)
    foreach(ui_file ${ui_files})
        get_filename_component(ui_name ${ui_file} NAME_WE)
        set(ui_header "${GENERATED_DIR}/ui_${ui_name}.h")
        list(APPEND generated_ui_headers ${ui_header})
        add_custom_command(
            OUTPUT ${ui_header}
            COMMAND "${QT_UIC}" "${CMAKE_CURRENT_SOURCE_DIR}/${ui_file}" -o "${ui_header}"
            MAIN_DEPENDENCY ${ui_file}
            COMMENT "UIC ${ui_file} -> ui_${ui_name}.h"
        )
    endforeach()

    # ----- MOC: Generate moc_*.cpp from headers with Q_OBJECT -----
    set(generated_moc_sources)
    foreach(hdr ${moc_headers})
        get_filename_component(hdr_name ${hdr} NAME_WE)
        set(moc_cpp "${GENERATED_DIR}/moc_${hdr_name}.cpp")
        list(APPEND generated_moc_sources ${moc_cpp})

        # MOC needs the same defines and includes as the compiler
        set(MOC_DEFINES
            -DUNICODE -DWIN32 -D_WIN32
            -DRENDERDOC_PLATFORM_WIN32
            -DSCINTILLA_QT=1 -DSCI_LEXER=1 -DMAKING_LIBRARY=1
            -DPYSIDE2_ENABLED=0
            -DQT_NO_DEBUG
            -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_CORE_LIB -DQT_SVG_LIB
        )

        set(MOC_INCLUDES
            -I"${CMAKE_CURRENT_SOURCE_DIR}"
            -I"${CMAKE_SOURCE_DIR}/renderdoc/api/replay"
            -I"${QT_INCLUDE_DIR}"
            -I"${QT_INCLUDE_DIR}/QtWidgets"
            -I"${QT_INCLUDE_DIR}/QtGui"
            -I"${QT_INCLUDE_DIR}/QtCore"
            -I"${QT_INCLUDE_DIR}/QtSvg"
            -I"${QT_INCLUDE_DIR}/QtNetwork"
        )

        add_custom_command(
            OUTPUT ${moc_cpp}
            COMMAND "${QT_MOC}" ${MOC_DEFINES} ${MOC_INCLUDES}
                "${CMAKE_CURRENT_SOURCE_DIR}/${hdr}" -o "${moc_cpp}"
            MAIN_DEPENDENCY ${hdr}
            COMMENT "MOC ${hdr} -> moc_${hdr_name}.cpp"
        )
    endforeach()

    # ----- RCC: Generate qrc_*.cpp from .qrc files -----
    set(generated_rcc_sources)
    foreach(qrc_file ${qrc_files})
        get_filename_component(qrc_name ${qrc_file} NAME_WE)
        set(rcc_cpp "${GENERATED_DIR}/qrc_${qrc_name}.cpp")
        list(APPEND generated_rcc_sources ${rcc_cpp})
        add_custom_command(
            OUTPUT ${rcc_cpp}
            COMMAND "${QT_RCC}" -name ${qrc_name}
                "${CMAKE_CURRENT_SOURCE_DIR}/${qrc_file}" -o "${rcc_cpp}"
            MAIN_DEPENDENCY ${qrc_file}
            COMMENT "RCC ${qrc_file} -> qrc_${qrc_name}.cpp"
        )
    endforeach()

    # ----- Scintilla include paths -----
    set(SCINTILLA_INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/scintilla/include
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/scintilla/include/qt
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/scintilla/src
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/scintilla/lexlib
    )

    # ----- Generate SWIG stub (when SWIG is not available) -----
    file(WRITE "${GENERATED_DIR}/swig_stubs.cpp"
        "// Auto-generated SWIG stubs for CMake build without SWIG\n"
        "#include <stddef.h>\n"
        "class QWidget;\n"
        "typedef struct _object PyObject;\n"
        "class rdcstr;\n"
        "bool CheckCoreInterface(rdcstr &log) { return true; }\n"
        "bool CheckQtInterface(rdcstr &log) { return true; }\n"
        "extern \"C\" PyObject *PyInit_renderdoc(void) { return NULL; }\n"
        "extern \"C\" PyObject *PassObjectToPython(const char *, void *) { return NULL; }\n"
        "extern \"C\" PyObject *PassNewObjectToPython(const char *, void *) { return NULL; }\n"
        "extern \"C\" PyObject *PyInit_qrenderdoc(void) { return NULL; }\n"
        "extern \"C\" PyObject *WrapBareQWidget(QWidget *) { return NULL; }\n"
        "extern \"C\" QWidget *UnwrapBareQWidget(PyObject *) { return NULL; }\n"
    )

    # ----- Precompiled header -----
    set(PCH_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/Code/precompiled.h")

    # ----- Build the executable -----
    add_executable(qrenderdoc WIN32
        ${qrenderdoc_sources}
        ${scintilla_sources}
        ${generated_moc_sources}
        ${generated_rcc_sources}
        ${generated_ui_headers}
        ${CMAKE_CURRENT_SOURCE_DIR}/Resources/qrenderdoc.rc
    )

    # Include directories
    target_include_directories(qrenderdoc PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty"
        "${GENERATED_DIR}"
        "${CMAKE_SOURCE_DIR}/renderdoc/api/replay"
        "${QT_INCLUDE_DIR}"
        "${QT_INCLUDE_DIR}/QtWidgets"
        "${QT_INCLUDE_DIR}/QtGui"
        "${QT_INCLUDE_DIR}/QtCore"
        "${QT_INCLUDE_DIR}/QtSvg"
        "${QT_INCLUDE_DIR}/QtNetwork"
        ${SCINTILLA_INCLUDES}
    )

    # Python - use the bundled embedded Python 3.6 in 3rdparty/python
    set(PYTHON3_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/python")
    set(PYTHON3_INCLUDE_DIR "${PYTHON3_ROOT}/include")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(PYTHON3_LIB_DIR "${PYTHON3_ROOT}/x64")
    else()
        set(PYTHON3_LIB_DIR "${PYTHON3_ROOT}/Win32")
    endif()

    # Python include path (needed for PythonContext.cpp)
    target_include_directories(qrenderdoc PRIVATE ${PYTHON3_INCLUDE_DIR})

    # Link the bundled Python import library
    target_link_libraries(qrenderdoc PRIVATE "${PYTHON3_LIB_DIR}/python36.lib")

    # Copy Python DLL and ZIP to output for embedded Python initialization
    add_custom_command(TARGET qrenderdoc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PYTHON3_LIB_DIR}/python36.dll" "$<TARGET_FILE_DIR:qrenderdoc>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PYTHON3_ROOT}/python36.zip" "$<TARGET_FILE_DIR:qrenderdoc>"
    )

    # Set the output directory to bin/
    set_target_properties(qrenderdoc PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    # Compile definitions (must be before link libraries)
    target_compile_definitions(qrenderdoc PRIVATE
        SCINTILLA_QT=1 SCI_LEXER=1 MAKING_LIBRARY=1
        PYSIDE2_ENABLED=0
        QT_WIDGETS_LIB QT_GUI_LIB QT_CORE_LIB QT_SVG_LIB
    )

    # Link Qt libraries (release variants only, bundled Qt has no debug libs)
    target_link_libraries(qrenderdoc PRIVATE
        "${QT_LIB_DIR}/qtmain.lib"
        "${QT_LIB_DIR}/Qt5Core.lib"
        "${QT_LIB_DIR}/Qt5Gui.lib"
        "${QT_LIB_DIR}/Qt5Widgets.lib"
        "${QT_LIB_DIR}/Qt5Svg.lib"
        "${QT_LIB_DIR}/Qt5Network.lib"
        renderdoc
        user32.lib
        shell32.lib
    )

    # MSVC compiler settings
    if(MSVC)
        target_compile_options(qrenderdoc PRIVATE
            /W4 /w44062 /w44840
            /wd4100 /wd4127 /wd4189 /wd4714 /wd4718 /wd4996
            /Zc:strictStrings /Zc:throwingNew
        )
        if(NOT RELEASE_MODE)
            target_compile_options(qrenderdoc PRIVATE /WX)
        endif()

        # Precompiled header - force include for most files
        target_compile_options(qrenderdoc PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:/FI${PCH_HEADER}>)

        # Force /MD runtime (qtmain.lib and Qt libs are compiled with /MD)
        target_compile_options(qrenderdoc PRIVATE /MD)

        # Scintilla files must NOT use PCH
        set_source_files_properties(${scintilla_sources} PROPERTIES
            COMPILE_FLAGS "/wd4244 /wd4458 /D_CRT_SECURE_NO_WARNINGS")

        # Additional include dirs for scintilla (per-file)
        foreach(scintilla_src ${scintilla_sources})
            set_source_files_properties(${scintilla_src}
                APPEND PROPERTY INCLUDE_DIRECTORIES ${SCINTILLA_INCLUDES})
        endforeach()

        # Linker
        target_link_options(qrenderdoc PRIVATE
            /SUBSYSTEM:WINDOWS
            /ignore:4099
        )

        # Copy Qt DLLs to output
        add_custom_command(TARGET qrenderdoc POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QT_BIN_DIR}/Qt5Core.dll" "$<TARGET_FILE_DIR:qrenderdoc>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QT_BIN_DIR}/Qt5Gui.dll" "$<TARGET_FILE_DIR:qrenderdoc>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QT_BIN_DIR}/Qt5Widgets.dll" "$<TARGET_FILE_DIR:qrenderdoc>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QT_BIN_DIR}/Qt5Svg.dll" "$<TARGET_FILE_DIR:qrenderdoc>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QT_BIN_DIR}/Qt5Network.dll" "$<TARGET_FILE_DIR:qrenderdoc>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/Resources/qt.conf"
                "$<TARGET_FILE_DIR:qrenderdoc>"
        )

        # Qt plugin deployment
        set(QT_PLUGINS_DIR "${QT_ROOT}/plugins")
        if(EXISTS "${QT_PLUGINS_DIR}/platforms/qwindows.dll")
            add_custom_command(TARGET qrenderdoc POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:qrenderdoc>/platforms"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${QT_PLUGINS_DIR}/platforms/qwindows.dll"
                    "$<TARGET_FILE_DIR:qrenderdoc>/platforms/qwindows.dll"
            )
        endif()

        if(EXISTS "${QT_PLUGINS_DIR}/styles/qwindowsvistastyle.dll")
            add_custom_command(TARGET qrenderdoc POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:qrenderdoc>/styles"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${QT_PLUGINS_DIR}/styles/qwindowsvistastyle.dll"
                    "$<TARGET_FILE_DIR:qrenderdoc>/styles/qwindowsvistastyle.dll"
            )
        endif()

        if(EXISTS "${QT_PLUGINS_DIR}/imageformats")
            add_custom_command(TARGET qrenderdoc POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${QT_PLUGINS_DIR}/imageformats"
                    "$<TARGET_FILE_DIR:qrenderdoc>/imageformats"
            )
        endif()

    endif()

    return()
endif()

if(ENABLE_QRENDERDOC)
    # ============================================================
    # Original qmake-based build for Linux/macOS
    # ============================================================

    # Only check qt version if we're building qrenderdoc
    execute_process(
        COMMAND ${QMAKE_QT5_COMMAND} -query QT_VERSION
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE QT_VERSION
        ERROR_VARIABLE QT_VERSION_STDERR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
        )

    # we need the trailing .999 since 5.5.1 would be VERSION_GREATER than 5.5 which implicitly means 5.5.0
    if (QT_VERSION VERSION_GREATER 5.5.999)
        message(STATUS "Building using Qt ${QT_VERSION}")
    else()
        message(FATAL_ERROR "${QMAKE_QT5_COMMAND} -query QT_VERSION returned: \"${QT_VERSION}\". Error output was: \"${QT_VERSION_STDERR}\". We require Qt >= 5.6 and for the qmake executable to run Qt5. Check your distribution for how to configure Qt5, you may need to set the QT_SELECT environment variable or customise the cmake variable QMAKE_QT5_COMMAND to qmake-qt5")
    endif()

endif()

# On Windows, skip SWIG/bison/make setup (only needed for the qmake-based build)
if(WIN32)
    if(ENABLE_PYRENDERDOC)
        add_subdirectory(Code/pyrenderdoc)
    endif()
    return()
endif()

set(QMAKE_CONFIG "debug")
set(QMAKE_LDFLAGS "")
set(QMAKE_CXXFLAGS "")
set(QMAKE_RPATHDIR "")
set(SWIG_FLAGS "")

if(ENABLE_ASAN)
    set (QMAKE_CXXFLAGS "-fsanitize=address -fno-omit-frame-pointer")
    set (QMAKE_LDFLAGS "-fsanitize=address")
endif()

if(ENABLE_TSAN)
    set (QMAKE_CXXFLAGS "-fsanitize=thread -fno-omit-frame-pointer")
    set (QMAKE_LDFLAGS "-fsanitize=thread")
endif()

if(STATIC_QRENDERDOC)
    set(QMAKE_CXXFLAGS "-DSTATIC_QRENDERDOC=1")
    set(QMAKE_CONFIG "${QMAKE_CONFIG} static")
    set(QMAKE_LDFLAGS "-lutil")
endif()

if(NOT APPLE)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8) # 64-bit
        set(SWIG_FLAGS "-DSWIGWORDSIZE64")
    else() # 32-bit
        set(SWIG_FLAGS "-DSWIGWORDSIZE32")
    endif()
endif()

if(RELEASE_MODE)
    set(QMAKE_CONFIG "release")
    set(QMAKE_CXXFLAGS "-DRELEASE=1 ${QMAKE_CXXFLAGS}")
endif()

if(QRENDERDOC_NO_CXX11_REGEX)
    set(QMAKE_CXXFLAGS "-DNO_CXX11_REGEX=1 ${QMAKE_CXXFLAGS}")
endif()

if(ENABLE_GL)
    set(QMAKE_CXXFLAGS "${QMAKE_CXXFLAGS} -DRENDERDOC_SUPPORT_GL")
endif()

if(ENABLE_GLES)
    set(QMAKE_CXXFLAGS "${QMAKE_CXXFLAGS} -DRENDERDOC_SUPPORT_GLES")
endif()

if(ENABLE_VULKAN)
    set(QMAKE_CXXFLAGS "${QMAKE_CXXFLAGS} -DRENDERDOC_SUPPORT_VULKAN")
endif()

if(ENABLE_METAL)
    set(QMAKE_CXXFLAGS "${QMAKE_CXXFLAGS} -DRENDERDOC_SUPPORT_METAL")
endif()

if(APPLE)
    set(QMAKE_RPATHDIR "@executable_path/../lib")
    add_custom_command(OUTPUT RenderDoc.icns
        COMMAND echo Creating OS X Icon
        COMMAND mkdir -p RenderDoc.iconset
        COMMAND qlmanage -t -s 1024 -o . ${CMAKE_CURRENT_SOURCE_DIR}/Resources/logo.svg
        COMMAND sips -z 16 16     logo.svg.png --out RenderDoc.iconset/icon_16x16.png
        COMMAND sips -z 32 32     logo.svg.png --out RenderDoc.iconset/icon_16x16@2x.png
        COMMAND sips -z 32 32     logo.svg.png --out RenderDoc.iconset/icon_32x32.png
        COMMAND sips -z 64 64     logo.svg.png --out RenderDoc.iconset/icon_32x32@2x.png
        COMMAND sips -z 128 128   logo.svg.png --out RenderDoc.iconset/icon_128x128.png
        COMMAND sips -z 256 256   logo.svg.png --out RenderDoc.iconset/icon_128x128@2x.png
        COMMAND sips -z 256 256   logo.svg.png --out RenderDoc.iconset/icon_256x256.png
        COMMAND sips -z 512 512   logo.svg.png --out RenderDoc.iconset/icon_256x256@2x.png
        COMMAND sips -z 1024 1024 logo.svg.png --out RenderDoc.iconset/icon_512x512.png
        COMMAND iconutil -c icns RenderDoc.iconset
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Resources/logo.svg)
else()
    add_custom_command(OUTPUT RenderDoc.icns COMMAND touch RenderDoc.icns)
endif()

include(ExternalProject)
include(CheckIncludeFile)

# Need bison for swig
find_package(BISON)

if(APPLE)
    # macOS 10.14+ ships broken compilers, need to disable CC/CXX inheritance
    # If SWIG configuration fails, try configuring CMake with these vars set to clang and clang++ respectively
    if (NOT DEFINED SWIG_CONFIGURE_CC)
        set(SWIG_CONFIGURE_CC "")
    endif()
    if (NOT DEFINED SWIG_CONFIGURE_CXX)
        set(SWIG_CONFIGURE_CXX "")
    endif()
else()
    set(SWIG_CONFIGURE_CC ${CMAKE_C_COMPILER})
    set(SWIG_CONFIGURE_CXX ${CMAKE_CXX_COMPILER})
endif()

set( GENERATOR_MAKE "$(MAKE)" )
set( GENERATOR_MAKE_PARAMS "" )
if( NOT CMAKE_MAKE_PROGRAM MATCHES "make$" )
    message( "Non make generator detected. Some projects compiled using add_custom_command like swig and QRenderDoc target will still be using make" )
    set( GENERATOR_MAKE "make" )

    include(ProcessorCount)
    ProcessorCount(PROC_COUNT)
    if(NOT PROC_COUNT EQUAL 0)
        message( "Running make with -j${PROC_COUNT} for parallel build" )
        set(GENERATOR_MAKE_PARAMS "-j${PROC_COUNT}")
    else()
        message(WARNING "!!!! Couldn't determine processor count. Make will be run non-parallel !!!!")
    endif()
endif()

if(APPLE AND CMAKE_GENERATOR STREQUAL "Xcode")
    if("${SHELL_CAPTURED_PATH}" STREQUAL "")
        set(SHELL_CAPTURED_PATH $ENV{PATH} CACHE STRING "Shell PATH" FORCE)
    endif()
    message(STATUS "Using custom path for targets custom_swig and QRenderDoc")
    set(SET_SYSTEM_PATH_COMMAND ${CMAKE_COMMAND} -E env PATH=${SHELL_CAPTURED_PATH})
endif()

if(NOT ${CMAKE_VERSION} VERSION_LESS "3.24")
    cmake_policy(SET CMP0135 NEW)
endif()

CHECK_INCLUDE_FILE(pcre.h PCRE_HEADER)

set(PCRE_PREFIX_PATH "")

if(PCRE_HEADER)
    message(STATUS "pcre library is found, using it as-is")

    add_custom_target(local_pcre)
else()
    message(STATUS "pcre header is not found! building pcre locally")

    # fetch pcre if missing
    ExternalProject_Add(local_pcre
        # using an URL to a zip directly so we don't clone the history etc
        URL ${RENDERDOC_PCRE_PACKAGE}
        BUILD_IN_SOURCE 1
        CONFIGURE_COMMAND CC=${SWIG_CONFIGURE_CC} CXX=${SWIG_CONFIGURE_CXX} CFLAGS=-fPIC CXXFLAGS=-fPIC ${SET_SYSTEM_PATH_COMMAND} ./configure --prefix=${CMAKE_BINARY_DIR} --disable-shared > /dev/null
        BUILD_COMMAND ${GENERATOR_MAKE} ${GENERATOR_MAKE_PARAMS} > /dev/null 2>&1
        INSTALL_COMMAND ${GENERATOR_MAKE} install > /dev/null 2>&1)

    set(PCRE_PREFIX_PATH "--with-pcre-prefix=${CMAKE_BINARY_DIR}")
endif()

# Compile our custom SWIG that will do scoped/strong enum classes
ExternalProject_Add(custom_swig
    # using an URL to a zip directly so we don't clone the history etc
    URL ${RENDERDOC_SWIG_PACKAGE}
    DEPENDS local_pcre
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ${SET_SYSTEM_PATH_COMMAND} ./autogen.sh > /dev/null 2>&1
    COMMAND CC=${SWIG_CONFIGURE_CC} CXX=${SWIG_CONFIGURE_CXX} CFLAGS=-fPIC CXXFLAGS=-fPIC ${SET_SYSTEM_PATH_COMMAND} ./configure --with-pcre=yes ${PCRE_PREFIX_PATH} --prefix=${CMAKE_BINARY_DIR} > /dev/null
    BUILD_COMMAND ${GENERATOR_MAKE} ${GENERATOR_MAKE_PARAMS} > /dev/null 2>&1
    INSTALL_COMMAND ${GENERATOR_MAKE} install > /dev/null 2>&1)

find_package(Shiboken2 QUIET)
find_package(PySide2 QUIET)

set(PYTHON_LINK "${PYTHON_LIBRARY}")

# ensure we link the whole python library so that modules have all the exports they need
if(STATIC_QRENDERDOC)
    set(PYTHON_LINK "-rdynamic -Wl,--whole-archive ${PYTHON_LINK} -Wl,--no-whole-archive")
endif()

# Output our configuration for qmake. We output this to a separate file so that
# the user can then open the qrenderdoc.pro in qt creator and be able to build
# with these configuration entries propagated for e.g. linking against libraries
# and finding dependencies from the cmake build
file(WRITE
     ${CMAKE_BINARY_DIR}/qrenderdoc/qrenderdoc_cmake.pri
     "CONFIG+=${QMAKE_CONFIG}\n"
     "\n"
     "QMAKE_CC=${CMAKE_C_COMPILER}\n"
     "QMAKE_CXX=${CMAKE_CXX_COMPILER}\n"
     "QMAKE_LINK=${CMAKE_CXX_COMPILER}\n"
     "QMAKE_CXXFLAGS+=${warning_flags}\n"
     "QMAKE_CXXFLAGS+=${QMAKE_CXXFLAGS}\n"
     "QMAKE_LFLAGS+=${QMAKE_LDFLAGS}\n"
     "QMAKE_RPATHDIR+=${QMAKE_RPATHDIR}\n"
     "\n"
     "LIB_SUFFIX=${LIB_SUFFIX}\n"
     "LIB_SUBFOLDER_TRAIL_SLASH=${LIB_SUBFOLDER_TRAIL_SLASH}\n"
     "\n"
     # search for -lrenderdoc here
     "LIBS+=-L${CMAKE_LIBRARY_OUTPUT_DIRECTORY}\n"
     "\n"
     # include and link against python
     "INCLUDEPATH+=${PYTHON_INCLUDE_DIR}\n"
     "LIBS+=${PYTHON_LINK}\n"
     "\n"
     "RENDERDOC_VERSION=${RENDERDOC_VERSION}\n"
     "\n")

if(APPLE)
    file(APPEND
        ${CMAKE_BINARY_DIR}/qrenderdoc/qrenderdoc_cmake.pri
        "OSX_ICONFILE=${CMAKE_CURRENT_BINARY_DIR}/RenderDoc.icns\n"
        "QMAKE_MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}\n")
endif()

# Ignore warnings - qmake has no way to do this per-file, so we must do it globally
if(CMAKE_COMPILER_IS_GNUCXX)
    file(APPEND
        ${CMAKE_BINARY_DIR}/qrenderdoc/qrenderdoc_cmake.pri
        "QMAKE_CXXFLAGS+=-Wno-unknown-warning -Wno-cast-function-type -Wno-stringop-truncation\n")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 14.0)
        file(APPEND
            ${CMAKE_BINARY_DIR}/qrenderdoc/qrenderdoc_cmake.pri
            "QMAKE_CXXFLAGS += -Wno-template-id-cdtor\n")
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    file(APPEND
        ${CMAKE_BINARY_DIR}/qrenderdoc/qrenderdoc_cmake.pri
        "QMAKE_CXXFLAGS+=-Wno-shadow -Wno-shorten-64-to-32\n")

    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 3.9)
        file(APPEND
            ${CMAKE_BINARY_DIR}/qrenderdoc/qrenderdoc_cmake.pri
            "QMAKE_CXXFLAGS+=-Wno-comma\n")
    endif()

    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 19)
        file(APPEND
            ${CMAKE_BINARY_DIR}/qrenderdoc/qrenderdoc_cmake.pri
            "QMAKE_CXXFLAGS+=-Wno-cast-function-type\n")
    endif()
endif()


if(ENABLE_UNSUPPORTED_EXPERIMENTAL_POSSIBLY_BROKEN_WAYLAND)
    message(WARNING "!!!! Using the Wayland Qt platform in qrenderdoc is NOT SUPPORTED !!!!")
    file(APPEND
        ${CMAKE_BINARY_DIR}/qrenderdoc/qrenderdoc_cmake.pri
        "DEFINES+=RENDERDOC_WINDOWING_WAYLAND\n")
endif()

# propagate build version info. Lots of escaping needed here to pass ""s into the define value
if(BUILD_VERSION_STABLE)
    file(APPEND
         ${CMAKE_BINARY_DIR}/qrenderdoc/qrenderdoc_cmake.pri
         "DEFINES+=RENDERDOC_STABLE_BUILD=1\n")
endif()

if(NOT BUILD_VERSION_DIST_NAME STREQUAL "")
    file(APPEND
         ${CMAKE_BINARY_DIR}/qrenderdoc/qrenderdoc_cmake.pri
         "DEFINES+=DISTRIBUTION_NAME='\\\\\"${BUILD_VERSION_DIST_NAME}\\\\\"'\n")
endif()

if(NOT BUILD_VERSION_DIST_VER STREQUAL "")
    file(APPEND
         ${CMAKE_BINARY_DIR}/qrenderdoc/qrenderdoc_cmake.pri
         "DEFINES+=DISTRIBUTION_VERSION='\\\\\"${BUILD_VERSION_DIST_VER}\\\\\"'\n")
endif()

if(NOT BUILD_VERSION_DIST_CONTACT STREQUAL "")
    file(APPEND
         ${CMAKE_BINARY_DIR}/qrenderdoc/qrenderdoc_cmake.pri
         "DEFINES+=DISTRIBUTION_CONTACT='\\\\\"${BUILD_VERSION_DIST_CONTACT}\\\\\"'\n")
endif()

option(QRENDERDOC_ENABLE_PYSIDE2 "Enable PySide2 if found" ON)

if(NOT QRENDERDOC_ENABLE_PYSIDE2)
    message(STATUS "PySide2 integration disabled")
elseif(PySide2_FOUND AND Shiboken2_FOUND AND TARGET Shiboken2::libshiboken AND TARGET PySide2::pyside2)
    message(STATUS "Building with PySide2 ${PySide2_VERSION} from ${PySide2_DIR}")

    if(NOT PYTHONLIBS_VERSION_STRING MATCHES "${SHIBOKEN_PYTHON_VERSION_MAJOR}.${SHIBOKEN_PYTHON_VERSION_MINOR}")
        message(FATAL_ERROR "Shiboken2 that we found was linked against python ${SHIBOKEN_PYTHON_VERSION_MAJOR}.${SHIBOKEN_PYTHON_VERSION_MINOR} but the python library we're linking against is ${PYTHONLIBS_VERSION_STRING}. Ensure these versions are identical or disable PySide2 compilation with QRENDERDOC_ENABLE_PYSIDE2.")
    endif()

    get_target_property(SHIBOKEN2_INCLUDE_DIRS Shiboken2::libshiboken INTERFACE_INCLUDE_DIRECTORIES)
    get_target_property(PYSIDE2_INCLUDE_DIRS PySide2::pyside2 INTERFACE_INCLUDE_DIRECTORIES)

    # append each include dir
    foreach(DIR ${SHIBOKEN2_INCLUDE_DIRS})
        file(APPEND
             ${CMAKE_BINARY_DIR}/qrenderdoc/qrenderdoc_cmake.pri
             "INCLUDEPATH+=${DIR}\n")
    endforeach()

    foreach(DIR ${PYSIDE2_INCLUDE_DIRS})
        file(APPEND
             ${CMAKE_BINARY_DIR}/qrenderdoc/qrenderdoc_cmake.pri
             "INCLUDEPATH+=${DIR}\n")
    endforeach()

    # Add configuration for PySide2
    file(APPEND
         ${CMAKE_BINARY_DIR}/qrenderdoc/qrenderdoc_cmake.pri
         "DEFINES+=PYSIDE2_ENABLED=1\n"
         "DEFINES+=PYSIDE2_SYS_PATH=${PYSIDE_PYTHONPATH}\n"
         "LIBS+=-lshiboken2${SHIBOKEN_PYTHON_SHARED_LIBRARY_SUFFIX}\n")
else()
    message(STATUS "PySide2 not found - Qt will not be accessible in python scripting. See https://github.com/baldurk/renderdoc/wiki/PySide2")

    file(APPEND
         ${CMAKE_BINARY_DIR}/qrenderdoc/qrenderdoc_cmake.pri
         "DEFINES+=PYSIDE2_ENABLED=0\n")
endif()

# generate the SWIG interface files
set(swig_interfaces
    Code/pyrenderdoc/renderdoc.i
    Code/pyrenderdoc/qrenderdoc.i)

set(swig_output)

file(GLOB RDOC_REPLAY_FILES ${CMAKE_SOURCE_DIR}/renderdoc/api/replay/*.h)
file(GLOB QRD_INTERFACE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/Code/Interface/*.h)

list(SORT RDOC_REPLAY_FILES)
list(SORT QRD_INTERFACE_FILES)

foreach(in ${swig_interfaces})
    get_filename_component(swig_file ${in} NAME_WE)

    add_custom_command(OUTPUT ${swig_file}_python.cxx ${swig_file}.py
            COMMAND ${CMAKE_BINARY_DIR}/bin/swig -v -Wextra -Werror ${SWIG_FLAGS} -O -c++ -python -modern -interface ${swig_file} -modernargs -fastunpack -py3 -builtin -I${CMAKE_CURRENT_SOURCE_DIR} -I${CMAKE_SOURCE_DIR}/renderdoc/api/replay -outdir ${CMAKE_CURRENT_BINARY_DIR} -o ${CMAKE_CURRENT_BINARY_DIR}/${swig_file}_python.cxx ${CMAKE_CURRENT_SOURCE_DIR}/${in}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${in}
            DEPENDS custom_swig
            DEPENDS ${RDOC_REPLAY_FILES}
            DEPENDS ${QRD_INTERFACE_FILES})

    list(APPEND swig_output ${swig_file}_python.cxx)
endforeach()

add_custom_target(swig-bindings ALL DEPENDS ${swig_output})

if(ENABLE_QRENDERDOC)

set(QRD_DEPENDENCIES RenderDoc.icns)

if(APPLE AND CMAKE_GENERATOR STREQUAL "Xcode")
    file(GLOB_RECURSE CODE_FILES Code/*.h Code/*.cpp Code/*.mm Code/*.i)
    list(SORT CODE_FILES COMPARE FILE_BASENAME)
    list(APPEND QRD_XCODE_SOURCES ${CODE_FILES})

    file(GLOB_RECURSE STYLES_FILES Styles/*.h Styles/*.cpp)
    list(SORT STYLES_FILES COMPARE FILE_BASENAME)
    list(APPEND QRD_XCODE_SOURCES ${STYLES_FILES})

    file(GLOB_RECURSE WIDGETS_FILES Widgets/*.h Widgets/*.cpp)
    list(SORT WIDGETS_FILES COMPARE FILE_BASENAME)
    list(APPEND QRD_XCODE_SOURCES ${WIDGETS_FILES})

    file(GLOB_RECURSE WINDOWS_FILES Windows/*.h Windows/*.cpp Windows/*.ui)
    list(SORT WINDOWS_FILES COMPARE FILE_BASENAME)
    list(APPEND QRD_XCODE_SOURCES ${WINDOWS_FILES})

    file(GLOB_RECURSE THIRDPARTY_FILES
        3rdParty/toolwindowmanager/*.h
        3rdParty/toolwindowmanager/*.cpp
        3rdParty/flowlayout/*.h
        3rdParty/flowlayout/*.cpp
        3rdParty/scintilla/*.h
        3rdParty/scintilla/*.cxx
        3rdParty/scintilla/*.cpp)
    list(SORT THIRDPARTY_FILES COMPARE FILE_BASENAME)
    list(APPEND QRD_XCODE_SOURCES ${THIRDPARTY_FILES})

    list(APPEND QRD_DEPENDENCIES ${CMAKE_BINARY_DIR}/qrenderdoc/renderdoc_python.cxx)
    list(APPEND QRD_DEPENDENCIES ${CMAKE_BINARY_DIR}/qrenderdoc/qrenderdoc_python.cxx)
    list(APPEND QRD_DEPENDENCIES ${QRD_XCODE_SOURCES})
endif()

# The case here is deliberately not matching the executable name
# This means the custom command doesn't create this output file,
# which causes CMake to rerun this target every time so that Qt
# can do dependency checking and rebuild anything necessary.
# Xcode does not match this behaviour. Add broad brush file dependencies.
# Xcode has an option to match this behaviour (not exposed by cmake).
add_custom_command(OUTPUT QRenderDoc
    COMMAND ${SET_SYSTEM_PATH_COMMAND} ${QMAKE_QT5_COMMAND} "CMAKE_DIR=${CMAKE_BINARY_DIR}" ${CMAKE_CURRENT_SOURCE_DIR}
    COMMAND ${GENERATOR_MAKE} ${GENERATOR_MAKE_PARAMS}
    DEPENDS ${QRD_DEPENDENCIES})
add_custom_target(build-qrenderdoc ALL DEPENDS QRenderDoc DEPENDS renderdoc DEPENDS swig-bindings)

if(APPLE AND CMAKE_GENERATOR STREQUAL "Xcode")
    set_target_properties(build-qrenderdoc PROPERTIES XCODE_SCHEME_DEBUG_DOCUMENT_VERSIONING OFF)
    set_target_properties(build-qrenderdoc PROPERTIES XCODE_SCHEME_EXECUTABLE ${CMAKE_BINARY_DIR}/bin/qrenderdoc.app)
    target_sources(build-qrenderdoc PRIVATE ${QRD_XCODE_SOURCES})
    source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${CODE_FILES})
    source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${STYLES_FILES})
    source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${WIDGETS_FILES})
    source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${WINDOWS_FILES})
    source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${THIRDPARTY_FILES})
endif()

install (PROGRAMS ${CMAKE_BINARY_DIR}/bin/qrenderdoc DESTINATION bin)

# Install supporting files for file associations etc
install (FILES share/application-x-renderdoc-capture.svg DESTINATION share/icons/hicolor/scalable/mimetypes/)
install (FILES share/renderdoc-icon-16x16.xpm DESTINATION share/pixmaps/)
install (FILES share/renderdoc-icon-32x32.xpm DESTINATION share/pixmaps/)
install (FILES share/magic DESTINATION share/doc/renderdoc)
install (FILES share/menu DESTINATION share/menu RENAME renderdoc)
install (FILES share/renderdoc.desktop DESTINATION share/applications)
install (FILES share/renderdoc.thumbnailer DESTINATION share/thumbnailers)
install (FILES share/renderdoc-capture.xml DESTINATION share/mime/packages)

install (CODE "MESSAGE(\"You now need to update some caches.\")")
install (CODE "MESSAGE(\"e.g.\")")
install (CODE "MESSAGE(\"sudo update-desktop-database\")")
install (CODE "MESSAGE(\"sudo update-menus\")")
install (CODE "MESSAGE(\"sudo update-mime-database /usr/share/mime/\")")
install (CODE "MESSAGE(\"sudo gtk-update-icon-cache /usr/share/icons/hicolor/\")")
install (CODE "MESSAGE(\"NB: Your paths may vary.\")")

endif() # if(ENABLE_QRENDERDOC)

# Build python modules - primarily used for constructing documentation
if(ENABLE_PYRENDERDOC AND (UNIX OR WIN32))
    add_subdirectory(Code/pyrenderdoc)
endif()
