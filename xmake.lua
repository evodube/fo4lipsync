-- set minimum xmake version
set_xmakever("2.8.2")

-- includes
includes("extern/CommonLibF4")

-- set project
set_project("fo4lipsync")
set_version("0.1.0")
set_license("MIT")

-- set defaults
set_languages("c++23")
set_warnings("allextra")

-- add rules
add_rules("mode.debug", "mode.releasedbg")

-- define targets
target("fo4lipsync")
    -- add dependencies to target
    add_deps("commonlibf4-ng")

    -- add commonlibf4 plugin
    add_rules("commonlibf4-ng.plugin", {
        name = "fo4lipsync",
        author = "evodube",
        description = "Keeps Fallout 4 lip sync locked to the voice audio at any frame rate"
    })

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/PCH.h")
