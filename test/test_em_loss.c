#include "harness.h"

/* The planner's loss model must be the one the /deye page draws. Bins are the
 * device's real efficiency curve of 2026-11-20; the expected coefficients come
 * from running the page's own fit() (deye.html) on the same data. */
#include "../main/em_loss.c"

static const em_loss_bin_t DIS[] = {
{1494.9f,754.9f,23629},{4304.2f,2825.3f,38627},{11566.6f,9212.3f,65045},{126714.9f,107219.7f,537121},{68973.6f,61211.2f,218544},{40306.2f,36886.4f,99412},{47511.9f,44050.0f,98591},{25607.3f,24036.4f,45708},{22411.2f,21216.0f,34646},{15084.0f,14380.4f,20830},{10616.6f,10153.0f,13115},{10260.6f,9851.3f,11618},{9196.0f,8860.2f,9441},{7304.1f,7049.8f,6972},{6571.1f,6354.0f,5761},{8697.3f,8418.3f,7123},{4540.7f,4398.0f,3491},{4167.5f,4034.6f,2986},{3203.6f,3104.7f,2182},{2078.0f,2015.5f,1326},{11135.7f,10813.1f,6817},{11321.5f,10997.0f,6043},{7653.0f,7430.9f,3576},{787.3f,762.5f,318},{250.7f,243.0f,90},{1175.5f,1136.4f,317},{333.3f,321.5f,83},{1528.8f,1456.4f,276},{0.0f,0.0f,0}};
static const em_loss_bin_t CHG[] = {
{9043.1f,2557.5f,330174},{2019.0f,1172.4f,29274},{3004.2f,2102.7f,23536},{3976.4f,3175.4f,19423},{7436.8f,6473.5f,25779},{10268.2f,9202.5f,28334},{7850.2f,7168.7f,17528},{7061.0f,6515.1f,13365},{6834.6f,6364.4f,11194},{8206.4f,7670.2f,11885},{4129.2f,3877.0f,5331},{3962.4f,3731.6f,4625},{3576.6f,3379.7f,3815},{5386.9f,5115.1f,5254},{4770.6f,4533.8f,4316},{5524.2f,5261.3f,4667},{5662.7f,5399.9f,4464},{6148.1f,5869.4f,4567},{5593.6f,5353.6f,3917},{5739.0f,5487.0f,3795},{5943.3f,5692.0f,3732},{34689.3f,33246.9f,19183},{47191.0f,45296.1f,22252},{48843.2f,46808.6f,19989},{63264.5f,60521.0f,22200},{25897.1f,24753.3f,7306},{13649.2f,13019.0f,2938},{66220.5f,62853.9f,12401},{0.0f,0.0f,0}};

static void test_matches_the_page(void)
{
    float k[3];
    CHECK(em_loss_fit(DIS, 29, true, k));
    CHECK_F(k[0], 0.129321, 1e-4);
    CHECK_F(k[1], -0.007931, 1e-4);
    CHECK_F(k[2], 0.0026647, 1e-5);
    CHECK(em_loss_fit(CHG, 29, false, k));
    CHECK_F(k[0], 0.110537, 1e-4);
    CHECK_F(k[1], 0.015559, 1e-4);
    CHECK_F(k[2], 0.0015131, 1e-5);
}

static void test_too_little_data(void)
{
    float k[3] = { 9, 9, 9 };
    em_loss_bin_t two[] = { { 1000, 900, 3600 }, { 5000, 4800, 3600 } };
    CHECK(!em_loss_fit(two, 2, true, k));
    CHECK_F(k[0], 9, 0);                                   /* untouched */
    em_loss_bin_t narrow[] = { { 1000, 950, 3600 }, { 1200, 1140, 3600 }, { 1500, 1420, 3600 } };
    CHECK(!em_loss_fit(narrow, 3, true, k));               /* 1:1.5, not 1:3 */
    em_loss_bin_t empty[29] = { { 0 } };
    CHECK(!em_loss_fit(empty, 29, false, k));
}

static void test_negative_terms_are_dropped(void)
{
    /* loss = 0.05 + 0.01 P exactly (no quadratic part): c must come out >= 0 */
    em_loss_bin_t b[6];
    float ps[] = { 0.5f, 1, 2, 4, 8, 12 };
    for (int i = 0; i < 6; i++) {
        float p = ps[i], loss = 0.05f + 0.01f * p;            /* kW, discharge: in = p + loss */
        b[i].s = 3600; b[i].out_wh = p * 1000; b[i].in_wh = (p + loss) * 1000;
    }
    float k[3];
    CHECK(em_loss_fit(b, 6, true, k));
    CHECK(k[0] >= 0 && k[2] >= 0);
    CHECK_F(k[0], 0.05, 1e-3);
    CHECK_F(k[1], 0.01, 1e-3);
}

int main(void)
{
    RUN(test_matches_the_page);
    RUN(test_too_little_data);
    RUN(test_negative_terms_are_dropped);
    return t_report("em_loss");
}
