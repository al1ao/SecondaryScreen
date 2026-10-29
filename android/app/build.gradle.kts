plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}
android {
    namespace = "com.padscreen.client"
    compileSdk = 34
    defaultConfig {
        applicationId = "com.padscreen.client"
        minSdk = 26
        targetSdk = 34
        versionCode = 7
        versionName = "0.6.1"
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}
// Stage 1 uses platform Activity/SurfaceView only; no additional UI library is needed.
