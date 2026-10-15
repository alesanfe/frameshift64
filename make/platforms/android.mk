# ------------------------------------------------------------------------------
# Android
# ------------------------------------------------------------------------------
android: guard-region ## Gradle assembleDebug (platforms/android)
	@$(call heading,Android build ($(REGION)))
	@cd "$(ANDROID_DIR)" && \
	  if [[ -x ./gradlew ]]; then \
	    ./gradlew assembleDebug $(BUILD_ARGS); \
	  elif [[ -f ./gradlew.bat ]]; then \
	    ./gradlew.bat assembleDebug; \
	  else \
	    $(call failure,Gradle wrapper not found); exit 1; \
	  fi

android-install: android ## gradlew installDebug
	@cd "$(ANDROID_DIR)" && ./gradlew installDebug

android-test: ## gradlew testDebugUnitTest (+ connected tests if adb device)
	@cd "$(ANDROID_DIR)" && ./gradlew testDebugUnitTest && \
	  if command -v adb >/dev/null 2>&1 && adb get-state >/dev/null 2>&1; then \
	    ./gradlew connectedDebugAndroidTest; \
	  else \
	    printf "$(YELLOW)[WARN]$(RESET) no device — instrumentation skipped\n"; \
	  fi

