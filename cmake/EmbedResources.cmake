# 将一组文件嵌入为 C++ 字节数组（供 src/server/web_assets.h 使用）
#
# 用法（脚本模式）:
#   cmake -DOUTPUT=<out.cpp> -DBASE_DIR=<dir> -DINPUTS=<a;b;c> -P EmbedResources.cmake

if(NOT DEFINED OUTPUT OR NOT DEFINED BASE_DIR)
    message(FATAL_ERROR "EmbedResources.cmake requires OUTPUT and BASE_DIR")
endif()

set(content "// 自动生成文件：请勿手工编辑（由 cmake/EmbedResources.cmake 生成）\n")
string(APPEND content "#include \"server/web_assets.h\"\n\n")
string(APPEND content "namespace testhub {\nnamespace web_assets {\n\n")

set(index 0)
set(table "")
foreach(input IN LISTS INPUTS)
    file(RELATIVE_PATH rel "${BASE_DIR}" "${input}")
    file(READ "${input}" hex HEX)
    string(LENGTH "${hex}" hexlen)
    math(EXPR bytes "${hexlen} / 2")
    if(bytes EQUAL 0)
        set(array "0x00")
        set(bytes 0)
    else()
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," array "${hex}")
        # 每 24 字节换行，避免超长行
        string(REGEX REPLACE "(0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],0x[0-9a-f][0-9a-f],)" "\\1\n    " array "${array}")
    endif()
    string(APPEND content "static const unsigned char data_${index}[] = {\n    ${array}\n};\n\n")
    string(APPEND table "    {\"${rel}\", data_${index}, ${bytes}},\n")
    math(EXPR index "${index} + 1")
endforeach()

if(index EQUAL 0)
    string(APPEND content "const Asset kAssets[] = {{\"\", nullptr, 0}};\n")
    string(APPEND content "const std::size_t kAssetCount = 0;\n")
else()
    string(APPEND content "const Asset kAssets[] = {\n${table}};\n")
    string(APPEND content "const std::size_t kAssetCount = ${index};\n")
endif()
string(APPEND content "\n} // namespace web_assets\n} // namespace testhub\n")

file(WRITE "${OUTPUT}.tmp" "${content}")
configure_file("${OUTPUT}.tmp" "${OUTPUT}" COPYONLY)
file(REMOVE "${OUTPUT}.tmp")
