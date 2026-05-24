plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "com.hughson.rime"
    compileSdk = 36
    ndkVersion = "27.2.12479018"

    // Read the project version from the repo-root VERSION file so all four
    // platforms (Mac/Win/Android/Linux) share one source of truth per release.
    val glaciemVersion = rootProject.file("../VERSION").readText().trim()

    defaultConfig {
        applicationId = "com.hughson.rime"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = glaciemVersion
        buildConfigField("String", "GLACIEM_VERSION", "\"$glaciemVersion\"")

        ndk { abiFilters += "arm64-v8a" }

        // The keygen sources are C++; link the STL statically so the single
        // librime_jni.so stays self-contained.
        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=c++_static"
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }

    buildFeatures {
        compose = true
        buildConfig = true   // enable BuildConfig.GLACIEM_VERSION
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.7")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.7")

    val composeBom = platform("androidx.compose:compose-bom:2024.12.01")
    implementation(composeBom)
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")

    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
    implementation("com.google.zxing:core:3.5.3")
    // v1.1.12: QR scanner for the Send sheet. journeyapps wraps ZXing
    // in an Android-native ScannerActivity; no Google Play Services
    // dependency, so it works on Quest / non-GMS devices too.
    implementation("com.journeyapps:zxing-android-embedded:4.3.0") {
        isTransitive = false
    }
    implementation("androidx.appcompat:appcompat:1.7.0")  // zxing-embedded needs AppCompat
}
