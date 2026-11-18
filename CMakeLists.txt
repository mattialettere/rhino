cmake_minimum_required(VERSION 3.24)
project(rhinoceros_cad VERSION 0.1.0 LANGUAGES C CXX)

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
  src/core/ModelCompare.h
  src/commands/Engine.cpp
  src/commands/Engine.h
  src/commands/CmdCurves.cpp
  src/commands/CmdSurfacesSolids.cpp
  src/commands/CmdTransform.cpp
  src/commands/CmdEdit.cpp
  src/commands/CmdDocument.cpp src/commands/CmdMore.cpp
  src/commands/CmdAnnotation.cpp src/commands/Annotation.cpp src/commands/Annotation.h
  src/commands/CmdExtra.cpp)
target_include_directories(cadcore PUBLIC src)

# Motore geometrico opzionale: OpenCASCADE (booleane, raccordi, offset, ...)
find_package(OpenCASCADE QUIET)
if(OpenCASCADE_FOUND)
  message(STATUS "OpenCASCADE ${OpenCASCADE_VERSION}: comandi del motore geometrico attivi")
  target_sources(cadcore PRIVATE src/kernel/OcctConvert.cpp src/kernel/OcctConvert.h src/kernel/CmdKernel.cpp src/kernel/CmdCurveEdit.cpp)
  target_include_directories(cadcore PUBLIC ${OpenCASCADE_INCLUDE_DIR})
  target_link_libraries(cadcore PUBLIC TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo
    TKTopAlgo TKPrim TKBO TKShHealing TKBool TKFillet TKOffset TKMesh)
  target_compile_definitions(cadcore PUBLIC CAD_WITH_OCCT)
else()
  message(WARNING "OpenCASCADE non trovato: i comandi del motore geometrico non saranno disponibili")
endif()
if(MSVC)
  # windows.h (incluso da openNURBS) definisce le macro min/max.
  target_compile_definitions(cadcore PUBLIC NOMINMAX _USE_MATH_DEFINES)
  # sorgenti in UTF-8 (testi italiani, simboli); file grandi di OpenCASCADE
  target_compile_options(cadcore PUBLIC /utf-8 /bigobj)
  target_link_libraries(cadcore PUBLIC opennurbsStatic)
else()
  # zlib e openNURBS si richiamano a vicenda (zcalloc/zcfree): gruppo di link.
  target_link_libraries(cadcore PUBLIC "$<LINK_GROUP:RESCAN,opennurbsStatic,zlib>")
endif()

add_executable(3dm-info tools/3dm-info/main.cpp)
target_link_libraries(3dm-info PRIVATE cadcore)

add_executable(cad-console tools/cad-console/main.cpp)
target_link_libraries(cad-console PRIVATE cadcore)

# Interfaccia grafica (Qt 6, opzionale)
find_package(Qt6 QUIET COMPONENTS Widgets OpenGL OpenGLWidgets)
if(Qt6_FOUND)
  message(STATUS "Qt ${Qt6_VERSION}: interfaccia grafica rhino-ui attiva")
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
  add_executable(rhino-ui WIN32
    src/ui/main.cpp src/ui/MainWindow.cpp src/ui/MainWindow.h
    src/ui/Viewport.cpp src/ui/Viewport.h src/ui/Scene.cpp src/ui/Scene.h)
  target_link_libraries(rhino-ui PRIVATE cadcore Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
  if(MSVC)
    target_compile_options(rhino-ui PRIVATE /UWIN32)
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
