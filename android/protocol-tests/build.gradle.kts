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
        kotlin.include("com/padscreen/client/Protocol.kt", "com/padscreen/client/H264AccessUnit.kt", "com/padscreen/client/ProtocolSelfTest.kt")
    }
}
application { mainClass.set("com.padscreen.client.ProtocolSelfTestKt") }
