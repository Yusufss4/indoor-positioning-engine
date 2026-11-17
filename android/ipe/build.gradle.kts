// No version catalog in this file: the module can be included in another project's build as it is. The
// Android plugin's version comes from the including build (here the root build.gradle.kts).
plugins {
  id("com.android.library")
}

android {
  namespace = "io.github.yusufss4.ipe"
  compileSdk = 37
  ndkVersion = "28.2.13676358"

  defaultConfig {
    minSdk = 26
    testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    ndk {
      abiFilters += listOf("arm64-v8a", "x86_64")
    }
    externalNativeBuild {
      cmake {
        arguments += listOf("-DANDROID_STL=c++_static")
      }
    }
  }

  externalNativeBuild {
    cmake {
      path = file("src/main/cpp/CMakeLists.txt")
      version = "3.31.6"
    }
  }

  compileOptions {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
  }

  sourceSets {
    // The device test replays a recorded mall walk of the repository (ReplayTest).
    getByName("androidTest") {
      assets.directories.add("../../data/demo/msil/site1_F2")
    }
  }
}

kotlin {
  jvmToolchain(17)
}

dependencies {
  testImplementation("junit:junit:4.13.2")
  androidTestImplementation("androidx.test.ext:junit:1.3.0")
  androidTestImplementation("androidx.test:runner:1.7.0")
}
