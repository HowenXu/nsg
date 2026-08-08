package info.skyblond.nsp.ui

import java.util.Locale

/**
 * Lightweight localization that follows the system language.
 * Chinese locales show Chinese, everything else shows English. No manual switch.
 */
object L10n {
    val isChinese: Boolean =
        Locale.getDefault().language.equals("zh", ignoreCase = true)

    fun t(zh: String, en: String): String = if (isChinese) zh else en
}
