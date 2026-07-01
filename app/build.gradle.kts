plugins {
    id("com.android.application")
}

android {
    namespace = "com.aruked.kafkalauncher"
    compileSdk = 35
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "com.aruked.kafkalauncher"
        minSdk = 28
        targetSdk = 34
        versionCode = 1
        versionName = "0.2.0"

        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=none"
            }
        }
        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        prefab = true
    }

    buildTypes {
        debug {
            isDebuggable = true
            isJniDebuggable = true
        }

        release {
            signingConfig = signingConfigs.getByName("debug")
            isDebuggable = false
            isJniDebuggable = false
            isMinifyEnabled = false
            ndk {
                debugSymbolLevel = "SYMBOL_TABLE"
            }
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
}

dependencies {
    implementation("com.bytedance.android:shadowhook:2.0.0")
}
