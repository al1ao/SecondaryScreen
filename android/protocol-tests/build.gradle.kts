plugins {
    id("org.jetbrains.kotlin.jvm")
    application
}
java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}
tasks.withType<org.jetbrains.kotlin.gradle.tasks.KotlinCompile>().configureEach {
    kotlinOptions.jvmTarget = "17"
}
sourceSets {
    main {
        kotlin.srcDir("../app/src/main/java")
        kotlin.include("com/secondaryscreen/client/Protocol.kt", "com/secondaryscreen/client/H264AccessUnit.kt", "com/secondaryscreen/client/ProtocolSelfTest.kt", "com/secondaryscreen/client/ReconnectDelay.kt", "com/secondaryscreen/client/DisplayValues.kt", "com/secondaryscreen/client/DisplayValuesSelfTest.kt", "com/secondaryscreen/client/SessionStats.kt", "com/secondaryscreen/client/OverlayText.kt", "com/secondaryscreen/client/InputValues.kt", "com/secondaryscreen/client/ResolutionOptions.kt")
    }
}
application { mainClass.set("com.secondaryscreen.client.ProtocolSelfTestKt") }
