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
        // from the release tag in CI (-PversionName=1.2.3 -PversionCode=10203)
        versionCode = (findProperty("versionCode") as String?)?.toInt() ?: 1
        versionName = findProperty("versionName") as String? ?: "dev"

        ndk {
            // phones and tablets (older 32-bit ones too); the emulator
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
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
            assets.directories.add("../../third_party/fonts")   // Noto Sans and its licence
        }
    }

    // The release key comes from the environment (CI secrets, see README); without it the
    // release build is signed with the debug key, which can't update a properly signed install
    val keystore = System.getenv("EDIPLAYER_KEYSTORE")
    signingConfigs {
        if (keystore != null) {
            create("release") {
                storeFile = file(keystore)
                storePassword = System.getenv("EDIPLAYER_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("EDIPLAYER_KEY_ALIAS")
                keyPassword = System.getenv("EDIPLAYER_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.findByName("release") ?: signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
