cmake_minimum_required(VERSION 3.24)
project(kurva3d VERSION 0.8.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# openNURBS: libreria open source ufficiale di McNeel per leggere/scrivere .3dm.
# Fissata all'ultima release 8.x: il ramo principale e' gia' Rhino 9 (archivio v90),
# che il progetto non deve usare finche' gli obiettivi Rhino 8 non sono completi.
set(OPENNURBS_TAG "v8.35.26251.13001" CACHE STRING "Tag openNURBS (deve restare 8.x)")
set(OPENNURBS_BUILD_STATIC ON CACHE BOOL "" FORCE)
FetchContent_Declare(opennurbs
  GIT_REPOSITORY https://github.com/mcneel/opennurbs.git
  GIT_TAG        ${OPENNURBS_TAG}
  GIT_SHALLOW    TRUE)
# I test interni di openNURBS non servono: li spegniamo solo per la dipendenza.
set(_saved_bt ${BUILD_TESTING})
set(BUILD_TESTING OFF)
FetchContent_MakeAvailable(opennurbs)
set(BUILD_TESTING ${_saved_bt})

# Libreria del nucleo CAD
add_library(cadcore STATIC
  src/core/Document3dm.cpp
  src/core/Document3dm.h
  src/core/ModelCompare.cpp
  src/core/SubDLimit.cpp
  src/core/SubDLimit.h
  src/core/ModelCompare.h
  src/commands/Engine.cpp
  src/commands/Engine.h
  src/commands/CmdCurves.cpp
  src/commands/CmdSurfacesSolids.cpp
  src/commands/CmdTransform.cpp
  src/commands/CmdEdit.cpp
  src/commands/CmdDocument.cpp src/commands/CmdMore.cpp
  src/commands/CmdAnnotation.cpp src/commands/Annotation.cpp src/commands/Annotation.h
  src/commands/CmdExtra.cpp
  src/commands/CmdMaterial.cpp src/commands/Materials.h
  src/commands/Exchange.cpp src/commands/Exchange.h
  src/commands/CmdViews.cpp
  src/commands/CmdLayout.cpp
  src/commands/History.cpp
  src/nodes/Graph.cpp src/nodes/Graph.h src/nodes/Components.cpp
  src/collab/Patch.cpp src/collab/Patch.h
  src/print/Page.cpp src/print/Page.h src/print/Pdf.cpp
  src/render/Render.cpp src/render/Render.h src/render/CmdRender.cpp
  src/ui/Scene.cpp src/ui/Scene.h
  src/commands/CmdExtra2.cpp
  src/commands/Grips.cpp
  src/commands/CmdExtra3.cpp
  src/commands/CmdExtra4.cpp
  src/commands/CmdExtra5.cpp
  src/commands/CmdExtra6.cpp
  src/commands/Deform.cpp
  src/commands/Deform.h
  src/commands/Grips.h
  src/ai/Assistant.cpp
  src/ai/Assistant.h
  src/ai/Hardware.cpp)
target_include_directories(cadcore PUBLIC src)

# Motore geometrico opzionale: OpenCASCADE (booleane, raccordi, offset, ...)
find_package(OpenCASCADE QUIET)
if(OpenCASCADE_FOUND)
  message(STATUS "OpenCASCADE ${OpenCASCADE_VERSION}: comandi del motore geometrico attivi")
  target_sources(cadcore PRIVATE src/kernel/OcctConvert.cpp src/kernel/OcctConvert.h src/kernel/CmdKernel.cpp src/kernel/CmdCurveEdit.cpp src/kernel/CmdSurfaceMore.cpp src/kernel/CmdSurface3.cpp src/kernel/CmdKernel4.cpp src/kernel/CmdKernel5.cpp src/kernel/CmdKernel6.cpp)
  target_include_directories(cadcore PUBLIC ${OpenCASCADE_INCLUDE_DIR})
  target_link_libraries(cadcore PUBLIC TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo
    TKTopAlgo TKPrim TKBO TKShHealing TKBool TKFillet TKOffset TKMesh TKHLR)
  target_compile_definitions(cadcore PUBLIC CAD_WITH_OCCT)
  # Scambio dati STEP/IGES: TKDESTEP/TKDEIGES da OpenCASCADE 7.8, TKSTEP/TKIGES prima
  set(_occt_de "")
  if(TARGET TKDESTEP AND TARGET TKDEIGES)
    set(_occt_de TKDESTEP TKDEIGES TKXSBase)
  elseif(TARGET TKSTEP AND TARGET TKIGES)
    set(_occt_de TKSTEP TKSTEPBase TKSTEPAttr TKSTEP209 TKIGES TKXSBase)
  endif()
  if(_occt_de)
    message(STATUS "OpenCASCADE DataExchange: STEP e IGES attivi (${_occt_de})")
    target_sources(cadcore PRIVATE src/kernel/StepIges.cpp)
    target_link_libraries(cadcore PUBLIC ${_occt_de})
    target_compile_definitions(cadcore PUBLIC CAD_WITH_OCCT_DE)
  else()
    message(WARNING "OpenCASCADE senza DataExchange: STEP e IGES non disponibili")
  endif()
else()
  message(WARNING "OpenCASCADE non trovato: i comandi del motore geometrico non saranno disponibili")
endif()
if(MSVC)
  # windows.h (incluso da openNURBS) definisce le macro min/max.
  target_compile_definitions(cadcore PUBLIC NOMINMAX _USE_MATH_DEFINES)
  # sorgenti in UTF-8 (testi italiani, simboli); file grandi di OpenCASCADE
  target_compile_options(cadcore PUBLIC /utf-8 /bigobj)
  target_link_libraries(cadcore PUBLIC opennurbsStatic)
  if(TARGET zlib)
    target_link_libraries(cadcore PUBLIC zlib)  # PDF compressi (Pdf.cpp)
  endif()
elseif(APPLE)
  # il linker di Apple risolve i riferimenti incrociati tra librerie statiche da solo
  target_link_libraries(cadcore PUBLIC opennurbsStatic zlib)
else()
  # zlib e openNURBS si richiamano a vicenda (zcalloc/zcfree): gruppo di link.
  target_link_libraries(cadcore PUBLIC "$<LINK_GROUP:RESCAN,opennurbsStatic,zlib>")
endif()

if(WIN32)
  target_link_libraries(cadcore PUBLIC dxgi)
endif()

# Script Python (opzionale): CPython incorporato e modulo rhinoscriptsyntax
option(KURVA3D_PYTHON "Script Python (RunPythonScript)" ON)
target_sources(cadcore PRIVATE src/python/PythonHost.cpp src/python/PythonHost.h)
# il sorgente di rhinoscriptsyntax.py diventa una stringa C++ (a pezzi: limite di MSVC sui letterali)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/python/rhinoscriptsyntax.py)
file(READ ${CMAKE_SOURCE_DIR}/src/python/rhinoscriptsyntax.py _rs_src)
string(LENGTH "${_rs_src}" _rs_len)
set(_rs_cpp "// generato da CMake da src/python/rhinoscriptsyntax.py\nnamespace cad {\nextern const char* const kRhinoscriptsyntax;\nconst char* const kRhinoscriptsyntax =\n")
set(_rs_pos 0)
while(_rs_pos LESS _rs_len)
  string(SUBSTRING "${_rs_src}" ${_rs_pos} 8000 _rs_chunk)
  string(APPEND _rs_cpp "R\"rs(${_rs_chunk})rs\"\n")
  math(EXPR _rs_pos "${_rs_pos} + 8000")
endwhile()
string(APPEND _rs_cpp ";\n}\n")
file(WRITE ${CMAKE_BINARY_DIR}/generated/RsSource.cpp.tmp "${_rs_cpp}")
file(COPY_FILE ${CMAKE_BINARY_DIR}/generated/RsSource.cpp.tmp ${CMAKE_BINARY_DIR}/generated/RsSource.cpp ONLY_IF_DIFFERENT)
target_sources(cadcore PRIVATE ${CMAKE_BINARY_DIR}/generated/RsSource.cpp)
if(KURVA3D_PYTHON)
  find_package(Python3 COMPONENTS Development.Embed)
  if(Python3_Development.Embed_FOUND)
    message(STATUS "Python ${Python3_VERSION}: script Python attivi")
    target_link_libraries(cadcore PUBLIC Python3::Python)
    target_compile_definitions(cadcore PUBLIC CAD_WITH_PYTHON)
  endif()
endif()

add_executable(3dm-info tools/3dm-info/main.cpp)
target_link_libraries(3dm-info PRIVATE cadcore)

add_executable(cad-console tools/cad-console/main.cpp)
target_link_libraries(cad-console PRIVATE cadcore)

# Interfaccia grafica (Qt 6, opzionale)
find_package(Qt6 QUIET COMPONENTS Widgets OpenGL OpenGLWidgets Network PrintSupport)
if(Qt6_FOUND)
  message(STATUS "Qt ${Qt6_VERSION}: interfaccia grafica kurva3d attiva")
  set(CMAKE_AUTOMOC ON)
  # Qt 6 su Windows aggiunge la macro WIN32 anche a 64 bit, ma openNURBS
  # si rifiuta di compilare con WIN32 definita in x64: la togliamo.
  if(WIN32 AND TARGET Qt6::Platform)
    get_target_property(_qt_defs Qt6::Platform INTERFACE_COMPILE_DEFINITIONS)
    if(_qt_defs)
      list(REMOVE_ITEM _qt_defs WIN32)
      set_target_properties(Qt6::Platform PROPERTIES INTERFACE_COMPILE_DEFINITIONS "${_qt_defs}")
    endif()
  endif()
  set(CMAKE_AUTORCC ON)
  # Nome del titolare dei diritti (dati versione dell'exe e finestra Informazioni)
  set(KURVA3D_PUBLISHER "Kurva3D" CACHE STRING "Titolare dei diritti di Kurva3D")
  set(_kurva_res res/kurva3d.qrc)
  if(WIN32)
    enable_language(RC)
    configure_file(res/kurva3d.rc.in ${CMAKE_BINARY_DIR}/kurva3d.rc @ONLY)
    list(APPEND _kurva_res ${CMAKE_BINARY_DIR}/kurva3d.rc)
  endif()
  if(APPLE)
    set(_icns ${CMAKE_SOURCE_DIR}/res/kurva3d.icns)
    set_source_files_properties(${_icns} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    list(APPEND _kurva_res ${_icns})
  endif()
  # Assistente AI (llama.cpp locale): servizi Qt condivisi da kurva3d e dal test kurva3d-ai-test
  add_library(kurva3d-ai STATIC src/aiqt/AiService.cpp src/aiqt/AiService.h)
  target_link_libraries(kurva3d-ai PUBLIC cadcore Qt6::Core Qt6::Network)
  # Collaborazione in rete locale (sessione, ricerca, sincronizzazione) e suo test con piu' sessioni
  add_library(kurva3d-collab STATIC src/collab/Session.cpp src/collab/Session.h)
  target_link_libraries(kurva3d-collab PUBLIC cadcore Qt6::Core Qt6::Network)
  add_executable(kurva3d-collab-test tools/collab-test/main.cpp)
  target_link_libraries(kurva3d-collab-test PRIVATE kurva3d-collab)
  if(MSVC)
    target_compile_options(kurva3d-collab PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/UWIN32>)
    target_compile_options(kurva3d-collab-test PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/UWIN32>)
  endif()
  add_executable(kurva3d-ai-test tools/ai-test/main.cpp)
  target_link_libraries(kurva3d-ai-test PRIVATE kurva3d-ai)
  if(MSVC)
    target_compile_options(kurva3d-ai PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/UWIN32>)
    target_compile_options(kurva3d-ai-test PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/UWIN32>)
  endif()
  add_executable(kurva3d WIN32 MACOSX_BUNDLE
    src/ui/AiPanel.cpp src/ui/AiPanel.h src/ui/TexturePanel.cpp src/ui/TexturePanel.h
    src/ui/main.cpp src/ui/MainWindow.cpp src/ui/MainWindow.h
    src/ui/Viewport.cpp src/ui/Viewport.h src/ui/LayoutView.cpp src/ui/LayoutView.h src/ui/RenderWindow.cpp src/ui/RenderWindow.h
    src/ui/ScriptEditor.cpp src/ui/ScriptEditor.h src/ui/NodeEditor.cpp src/ui/NodeEditor.h
    src/ui/CollabPanel.cpp src/ui/CollabPanel.h
    src/ui/Theme.cpp src/ui/Theme.h ${_kurva_res})
  if(APPLE)
    # versione minima di macOS: quella delle librerie Homebrew con cui si compila
    set(KURVA3D_MACOS_MIN "15.0" CACHE STRING "macOS minimo per Kurva3D.app")
    configure_file(res/Info.plist.in ${CMAKE_BINARY_DIR}/Info.plist @ONLY)
    set_target_properties(kurva3d PROPERTIES OUTPUT_NAME kurva3d
      MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/Info.plist)
  endif()
  target_compile_definitions(kurva3d PRIVATE $<$<COMPILE_LANGUAGE:CXX>:KURVA3D_PUBLISHER="${KURVA3D_PUBLISHER}">)
  target_compile_definitions(kurva3d PRIVATE $<$<COMPILE_LANGUAGE:CXX>:KURVA3D_VERSION="${PROJECT_VERSION}">)
  target_link_libraries(kurva3d PRIVATE cadcore kurva3d-ai kurva3d-collab Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::PrintSupport)
  if(MSVC)
    target_compile_options(kurva3d PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/UWIN32>)
  endif()
else()
  message(WARNING "Qt 6 non trovato: l'interfaccia grafica non verra' compilata")
endif()

add_executable(commands-test tests/commands_test.cpp)
target_link_libraries(commands-test PRIVATE cadcore)

add_executable(3dm-roundtrip tools/3dm-roundtrip/main.cpp)
target_link_libraries(3dm-roundtrip PRIVATE cadcore)

# Test: lettura -> scrittura v8 -> rilettura -> confronto, su tutti i file di esempio
enable_testing()
file(GLOB_RECURSE _sample_3dm "${opennurbs_SOURCE_DIR}/example_files/*.3dm")
foreach(f ${_sample_3dm})
  get_filename_component(_n ${f} NAME_WE)
  get_filename_component(_d ${f} DIRECTORY)
  get_filename_component(_v ${_d} NAME)
  add_test(NAME roundtrip_${_v}_${_n}
    COMMAND 3dm-roundtrip ${f} ${CMAKE_BINARY_DIR}/roundtrip_out/${_v}_${_n}.3dm)
endforeach()
add_test(NAME commands COMMAND commands-test ${CMAKE_BINARY_DIR}/commands_out)
if(TARGET kurva3d-collab-test)
  add_test(NAME collab COMMAND kurva3d-collab-test)
endif()

# Cartella per i file di Rhino 8 reali (test incrociati): tests/rhino8_files/*.3dm
file(GLOB _user_3dm "${CMAKE_SOURCE_DIR}/tests/rhino8_files/*.3dm")
foreach(f ${_user_3dm})
  get_filename_component(_n ${f} NAME_WE)
  add_test(NAME rhino8_${_n}
    COMMAND 3dm-roundtrip ${f} ${CMAKE_BINARY_DIR}/roundtrip_out/rhino8_${_n}.3dm)
endforeach()

# Differenze note (docs/DIFFERENZE.md): file di versioni antiche il cui
# andata/ritorno oggi NON e' identico. Sono segnati "WILL_FAIL" cosi' la suite
# resta verde ma, se un giorno passano, il test fallisce e ce ne accorgiamo.
set(KNOWN_ROUNDTRIP_DIFFERENCES
  roundtrip_V1_v1_Soccer          # D-004 ON_PolyCurve lette da Rhino 1
  roundtrip_V3_v3_Smooth          # D-005 B-rep di Rhino 3: 16 byte azzerati
  roundtrip_V3_v3_Speeder         # D-005
  roundtrip_V3_v3_SphericalSpiral # D-005
  roundtrip_V4_v4_example_file    # D-006 quote di Rhino 4/5 convertite
  roundtrip_V5_v5_example_file    # D-006
)
foreach(t ${KNOWN_ROUNDTRIP_DIFFERENCES})
  if(TEST ${t})
    set_tests_properties(${t} PROPERTIES WILL_FAIL TRUE LABELS "differenza-nota")
  endif()
endforeach()
