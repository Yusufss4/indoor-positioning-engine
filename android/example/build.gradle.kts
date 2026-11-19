plugins {
  alias(libs.plugins.android.application)
}

android {
  namespace = "io.github.yusufss4.ipe.example"
  compileSdk = 37

  defaultConfig {
    applicationId = "io.github.yusufss4.ipe.example"
    minSdk = 26
    targetSdk = 37
    versionCode = 1
    versionName = "0.3.0"
    ndk {
      abiFilters += listOf("arm64-v8a", "x86_64")
    }
  }

  compileOptions {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
  }

  sourceSets {
    // The repository's sample venues are the app's bundled venues.
    getByName("main") {
      assets.directories.add("../../data/venues")
    }
  }
}

kotlin {
  jvmToolchain(17)
}

dependencies {
  implementation(project(":ipe"))
  testImplementation(libs.junit)
}
