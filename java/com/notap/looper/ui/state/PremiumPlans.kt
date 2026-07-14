package com.notap.looper.ui.state

/**
 * SINGLE SOURCE OF TRUTH for premium pricing + value copy.
 *
 * The UI layer reads ONLY from here — no price string is hardcoded in a layout.
 * At Play Billing integration time, replace the placeholder `price` fields with
 * `ProductDetails.getFormattedPrice()` looked up by `productId` (localized,
 * store-authoritative). Nothing in MainActivity needs to change: swap the
 * values here (or make these `var`s populated from a BillingClient callback)
 * and the upgrade screen re-renders with real, localized prices.
 *
 * ⚠ The prices below are PLACEHOLDER MARKETING MOCK COPY — not real SKUs.
 */
object PremiumPlans {

    data class Plan(
        val productId: String,   // Play Console product id (wire to BillingClient)
        val title: String,
        val price: String,       // PLACEHOLDER — inject ProductDetails price at runtime
        val subtext: String,
        val tag: String?,        // e.g. "BEST VALUE"; null = no badge
        val highlighted: Boolean // the anchored hero card
    )

    // Decoy: visually subdued / struck-through to anchor the lifetime deal.
    val monthly = Plan(
        productId = "notap_pro_monthly",
        title = "Monthly Subscription",
        price = "$2.99",
        subtext = "Billed monthly · cancel anytime",
        tag = null,
        highlighted = false
    )

    // Hero: the value anchor.
    val lifetime = Plan(
        productId = "notap_pro_lifetime",
        title = "Lifetime Unlock",
        price = "$9.99",
        subtext = "One payment, yours forever.",
        tag = "BEST VALUE",
        highlighted = true
    )

    /** Ordered for display: decoy first (top), hero second (visually dominant). */
    val all = listOf(monthly, lifetime)

    /** Value proposition, headline → detail. */
    val valueProps = listOf(
        "Unlock Studio-Grade Tone" to "Full-depth reverb, beyond the free 30% taste.",
        "Octave Shifting" to "OCT± pitch effects layered on any loop.",
        "Everything We Ship Next" to "Every future effect and pro feature, included."
    )
}
