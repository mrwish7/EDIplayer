// The Android app: a NativeActivity running libediplayer.so, built by the
// repository's top-level CMakeLists.txt.
plugins {
    id("com.android.application")
}

android {
    namespace = "io.github.mrwish7.ediplayer"
    compileSdk = 36
    ndkVersion = "30.0.16248370"

    defaultConfig {
        applicationId = "io.github.mrwish7.ediplayer"
        minSdk = 29
        targetSdk = 36
        versionCode = 1
        versionName = "0.1"

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")   // phones and tablets; the emulator
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DEDIPLAYER_BUILD_TOOLS=OFF")
                targets += "ediplayer"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../../CMakeLists.txt")
            version = "3.31.6"
        }
    }

    sourceSets {
        getByName("main") {
            assets.srcDir("../../third_party/fonts")   // Noto Sans and its licence
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("debug")   // TODO: release key (phase 5)
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
