
env = SConscript('godot-cpp/SConstruct')

# Local dependency paths, adapt them to your setup
godot_headers_path = "godot-cpp/godot_headers/"
cpp_bindings_path = "godot-cpp/"
#cpp_library = "libgodot-cpp"


# Add JSB
jsb_headers_path = "jsbsim/src/"
jsb_library_path1 = "jsbsim/build/src/"
jsb_library_path2 = "jsbsim/src/"
jsb_library = "JSBSim"

env.Append(CPPPATH="src/")
env.Append(CPPPATH="godot-cpp/include/")
env.Append(CPPPATH="jsbsim/src/")
env.Append(CPPPATH=['.', godot_headers_path, jsb_headers_path, cpp_bindings_path + 'include/', cpp_bindings_path + 'include/core/', cpp_bindings_path + 'include/gen/'])
env.Append(LIBPATH=[cpp_bindings_path + 'bin/', jsb_library_path1, jsb_library_path2])
env.Append(LIBS=[jsb_library])

# On Windows, JSBSim is built as a static library so we must define JSBSIM_STATIC_LINK
# to avoid __declspec(dllimport) on JSBSim symbols.
import platform
if platform.system() == 'Windows':
    env.Append(CPPDEFINES=['JSBSIM_STATIC_LINK'])
    env.Append(LIBS=['ws2_32'])  # JSBSim uses sockets on Windows

#env.Append(LIBS=['JSBSim'])
#env.Append(LIBPATH=['/path/to/jsbsim/library'])  # Adjust the path
#env.Append(LIB="....")
#env.Append(LIBPATH="....")


src = Glob("src/*.cpp")

libpath = 'provpilot/bin/libprovpilot{}{}'.format( env['suffix'], env['SHLIBSUFFIX'] )
sharedlib = env.SharedLibrary(libpath,src)
Default(sharedlib)

