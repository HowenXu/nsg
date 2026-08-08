package info.skyblond.nsp.ui

import java.util.Locale

/**
 * 轻量级本地化：自动跟随系统语言。
 * 中文系统显示中文，其他语言显示英文，不提供手动选择。
 */
object L10n {
    val isChinese: Boolean =
        Locale.getDefault().language.equals("zh", ignoreCase = true)

    fun t(zh: String, en: String): String = if (isChinese) zh else en
}
