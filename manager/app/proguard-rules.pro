-verbose
-optimizationpasses 5

-dontwarn org.conscrypt.**
-dontwarn kotlinx.serialization.**

# Please add these rules to your existing keep rules in order to suppress warnings.
# This is generated automatically by the Android Gradle plugin.
-dontwarn com.google.auto.service.AutoService
-dontwarn com.google.j2objc.annotations.RetainedWith
-dontwarn javax.lang.model.SourceVersion
-dontwarn javax.lang.model.element.AnnotationMirror
-dontwarn javax.lang.model.element.AnnotationValue
-dontwarn javax.lang.model.element.Element
-dontwarn javax.lang.model.element.ElementKind
-dontwarn javax.lang.model.element.ElementVisitor
-dontwarn javax.lang.model.element.ExecutableElement
-dontwarn javax.lang.model.element.Modifier
-dontwarn javax.lang.model.element.Name
-dontwarn javax.lang.model.element.PackageElement
-dontwarn javax.lang.model.element.TypeElement
-dontwarn javax.lang.model.element.TypeParameterElement
-dontwarn javax.lang.model.element.VariableElement
-dontwarn javax.lang.model.type.ArrayType
-dontwarn javax.lang.model.type.DeclaredType
-dontwarn javax.lang.model.type.ExecutableType
-dontwarn javax.lang.model.type.TypeKind
-dontwarn javax.lang.model.type.TypeMirror
-dontwarn javax.lang.model.type.TypeVariable
-dontwarn javax.lang.model.type.TypeVisitor
-dontwarn javax.lang.model.util.AbstractAnnotationValueVisitor8
-dontwarn javax.lang.model.util.AbstractTypeVisitor8
-dontwarn javax.lang.model.util.ElementFilter
-dontwarn javax.lang.model.util.Elements
-dontwarn javax.lang.model.util.SimpleElementVisitor8
-dontwarn javax.lang.model.util.SimpleTypeVisitor7
-dontwarn javax.lang.model.util.SimpleTypeVisitor8
-dontwarn javax.lang.model.util.Types
-dontwarn javax.tools.Diagnostic$Kind


# MMRL:webui reflection
-keep class com.dergoogler.mmrl.webui.interfaces.** { *; }
-keep class org.rouyashiki.yukisu.ui.webui.WebViewInterface { *; }

-keep,allowobfuscation class * extends com.dergoogler.mmrl.platform.content.IService { *; }

# Module repository state is read by Gson through reflection. Keep the generic signatures and
# persisted model fields stable across release builds.
-keepattributes Signature
-keep class org.rouyashiki.yukisu.data.repository.ModuleRepositoryManager$PersistedState { *; }
-keep class org.rouyashiki.yukisu.data.repository.RepositoryFormat { *; }
-keep class org.rouyashiki.yukisu.data.repository.RepositorySource { *; }
-keep class org.rouyashiki.yukisu.data.repository.ModuleCompatibility { *; }
-keep class org.rouyashiki.yukisu.data.repository.RepositoryModuleVersion { *; }
-keep class org.rouyashiki.yukisu.data.repository.RepositoryModule { *; }
-keep class org.rouyashiki.yukisu.data.repository.RepositorySnapshot { *; }
-keep class org.rouyashiki.yukisu.data.repository.InstalledModuleBinding { *; }

# commons-compress reaches for zstd-jni, which we do not ship.
-dontwarn com.github.luben.zstd.**
