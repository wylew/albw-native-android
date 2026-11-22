// Android app: SDL3's Java glue (SDLActivity) + libmain.so.
// SDL sources come from build/deps/SDL (android/fetch_deps.sh, same release as build.sh).
plugins {
    id("com.android.application")
}

val repoRoot = rootDir.parentFile
val sdlDir = File(repoRoot, "build/deps/SDL")
if (!File(sdlDir, "CMakeLists.txt").exists()) {
    throw GradleException("SDL sources missing: run android/fetch_deps.sh first")
}
// The game: tools/link_android.sh -> build/android/jniLibs/<abi>/{libmain,libSDL3}.so.
// Without it (or with -Psmoke) the step-1 smoke test is built from src/main/cpp instead.
val gameLibs = File(repoRoot, "build/android/jniLibs")
val useGame = File(gameLibs, "armeabi-v7a/libmain.so").exists() && !project.hasProperty("smoke")

android {
    namespace = "org.albw.port"
    compileSdk = 35
    ndkVersion = "30.0.16248370"

    defaultConfig {
        applicationId = "org.albw.port"
        minSdk = 24
        targetSdk = 35
        versionCode = 1
        versionName = file("${repoRoot}/VERSION").readText().trim()
        ndk {
            // 32-bit ARM first (README: Android port, Approach)
            abiFilters += listOf("armeabi-v7a")
        }
        if (!useGame) {
            externalNativeBuild {
                cmake {
                    arguments += listOf("-DSDL_DIR=${sdlDir}", "-DANDROID_STL=c++_static")
                }
            }
        }
    }

    if (useGame) {
        sourceSets["main"].jniLibs.directories.add(gameLibs.path)
    } else {
        externalNativeBuild {
            cmake {
                path = file("src/main/cpp/CMakeLists.txt")
                version = "4.1.2"
            }
        }
    }

    sourceSets["main"].java.directories.add("${sdlDir}/android-project/app/src/main/java")

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("debug")
        }
    }
    // native libraries extracted to nativeLibraryDir (a loader may need real files, step 4)
    packaging {
        jniLibs.useLegacyPackaging = true
    }
    lint {
        abortOnError = false
    }
}
