using MusicPlayerLibrary;

namespace WpfMusicPlayer.Test;

[TestClass]
public sealed class LrcFileControllerTest
{
    private const string SimpleLrc =
        """
        [ar:TestArtist]
        [ti:TestTitle]
        [al:TestAlbum]
        [by:TestBy]
        [offset:0]
        [00:01.00]First line
        [00:05.50]Second line
        [00:10.00]Third line
        """;

    private static LrcFileController CreateFromStream(string lrc)
    {
        var controller = new LrcFileController();
        controller.SetSongDuration(60f);
        controller.ParseLrcStream(lrc);
        return controller;
    }

    #region Parse & Valid

    [TestMethod]
    public void ParseLrcStream_SimpleLrc_IsValid()
    {
        using var ctrl = CreateFromStream(SimpleLrc);
        Assert.IsTrue(ctrl.Valid());
    }

    [TestMethod]
    public void NewController_WithoutParsing_IsNotValid()
    {
        using var ctrl = new LrcFileController();
        Assert.IsFalse(ctrl.Valid());
    }

    [TestMethod]
    public void ParseLrcStream_SimpleLrc_NodeCountEquals3()
    {
        using var ctrl = CreateFromStream(SimpleLrc);
        Assert.AreEqual(3, ctrl.GetLrcNodeCount());
    }

    #endregion

    #region Node time

    [TestMethod]
    public void GetLrcNodeTimeMs_ReturnsCorrectTimestamps()
    {
        using var ctrl = CreateFromStream(SimpleLrc);

        Assert.AreEqual(1000, ctrl.GetLrcNodeTimeMs(0));
        Assert.AreEqual(5500, ctrl.GetLrcNodeTimeMs(1));
        Assert.AreEqual(10000, ctrl.GetLrcNodeTimeMs(2));
    }

    [TestMethod]
    public void GetLrcNodeTimeMs_TwoDigitMilliseconds_PadsCorrectly()
    {
        // [00:02.05] -> 05 is 2 digits, should be treated as 050 ms
        const string lrc =
            """
            [00:02.05]Line A
            [00:04.10]Line B
            """;

        using var ctrl = CreateFromStream(lrc);

        Assert.AreEqual(2050, ctrl.GetLrcNodeTimeMs(0));
        Assert.AreEqual(4100, ctrl.GetLrcNodeTimeMs(1));
    }

    [TestMethod]
    public void GetLrcNodeTimeMs_ThreeDigitMilliseconds()
    {
        const string lrc =
            """
            [00:03.123]Hello
            """;

        using var ctrl = CreateFromStream(lrc);

        Assert.AreEqual(3123, ctrl.GetLrcNodeTimeMs(0));
    }

    #endregion

    #region Lyric text

    [TestMethod]
    public void GetLrcLineAt_ReturnsCorrectText()
    {
        using var ctrl = CreateFromStream(SimpleLrc);

        Assert.AreEqual("First line", ctrl.GetLrcLineAt(0, 0));
        Assert.AreEqual("Second line", ctrl.GetLrcLineAt(1, 0));
        Assert.AreEqual("Third line", ctrl.GetLrcLineAt(2, 0));
    }

    [TestMethod]
    public void GetLrcLineAt_InvalidIndex_ThrowArgumentOutOfRangeException()
    {
        const string lrc =
            """
            [00:03.123]Hello
            """;
        using var ctrl = CreateFromStream(lrc);
        Assert.ThrowsExactly<ArgumentOutOfRangeException>(() => ctrl.GetLrcLineAt(1, 1));
    }

    #endregion

    #region Timestamp navigation

    [TestMethod]
    public void SetTimeStamp_UpdatesCurrentTimeStamp()
    {
        using var ctrl = CreateFromStream(SimpleLrc);
        ctrl.SetTimeStamp(3000);
        Assert.AreEqual(3000, ctrl.GetCurrentTimeStamp());
    }

    [TestMethod]
    public void SetTimeStamp_SelectsCorrectNodeIndex()
    {
        using var ctrl = CreateFromStream(SimpleLrc);

        ctrl.SetTimeStamp(0);
        Assert.AreEqual(0, ctrl.GetCurrentLrcNodeIndex());

        ctrl.SetTimeStamp(6000);
        Assert.AreEqual(1, ctrl.GetCurrentLrcNodeIndex());

        ctrl.SetTimeStamp(15000);
        Assert.AreEqual(2, ctrl.GetCurrentLrcNodeIndex());
    }

    [TestMethod]
    public void TimeStampIncrease_AdvancesTimestamp()
    {
        using var ctrl = CreateFromStream(SimpleLrc);
        ctrl.SetTimeStamp(1000);
        ctrl.TimeStampIncrease(5000);
        Assert.AreEqual(6000, ctrl.GetCurrentTimeStamp());
    }

    [TestMethod]
    public void GetCurrentLrcLineAt_MatchesSetTimeStamp()
    {
        using var ctrl = CreateFromStream(SimpleLrc);
        ctrl.SetTimeStamp(6000);

        Assert.AreEqual("Second line", ctrl.GetCurrentLrcLineAt(0));
    }

    [TestMethod]
    public void GetCurrentLrcLinesCount_SingleNode_Returns1()
    {
        using var ctrl = CreateFromStream(SimpleLrc);
        ctrl.SetTimeStamp(1500);
        Assert.AreEqual(1, ctrl.GetCurrentLrcLinesCount());
    }

    #endregion

    #region ClearLrcNodes

    [TestMethod]
    public void ClearLrcNodes_MakesControllerInvalid()
    {
        using var ctrl = CreateFromStream(SimpleLrc);
        Assert.IsTrue(ctrl.Valid());

        ctrl.ClearLrcNodes();
        Assert.IsFalse(ctrl.Valid());
    }

    #endregion

    #region AuxiliaryInfo

    [TestMethod]
    public void AuxiliaryInfo_SetAndClear()
    {
        using var ctrl = CreateFromStream(SimpleLrc);

        ctrl.SetAuxiliaryInfoEnabled(LrcAuxiliaryInfo.Translation);
        Assert.IsTrue(ctrl.IsAuxiliaryInfoEnabled(LrcAuxiliaryInfo.Translation));

        ctrl.ClearAuxiliaryInfoEnabled(LrcAuxiliaryInfo.Translation);
        Assert.IsFalse(ctrl.IsAuxiliaryInfoEnabled(LrcAuxiliaryInfo.Translation));
    }

    [TestMethod]
    public void ResetAuxiliaryInfoEnabled_ClearsAll()
    {
        using var ctrl = CreateFromStream(SimpleLrc);

        ctrl.SetAuxiliaryInfoEnabled(LrcAuxiliaryInfo.Translation);
        ctrl.SetAuxiliaryInfoEnabled(LrcAuxiliaryInfo.Romanization);
        ctrl.ResetAuxiliaryInfoEnabled();

        Assert.IsFalse(ctrl.IsAuxiliaryInfoEnabled(LrcAuxiliaryInfo.Translation));
        Assert.IsFalse(ctrl.IsAuxiliaryInfoEnabled(LrcAuxiliaryInfo.Romanization));
    }

    [TestMethod]
    public void GetLrcLineAuxIndex_SimpleLrc_LyricAt0()
    {
        using var ctrl = CreateFromStream(SimpleLrc);
        Assert.AreEqual(0, ctrl.GetLrcLineAuxIndex(0, LrcAuxiliaryInfo.Lyric));
    }

    [TestMethod]
    public void GetCurrentLrcLineAuxIndex_ReturnsCorrectIndex()
    {
        using var ctrl = CreateFromStream(SimpleLrc);
        ctrl.SetTimeStamp(2000);
        Assert.AreEqual(0, ctrl.GetCurrentLrcLineAuxIndex(LrcAuxiliaryInfo.Lyric));
        Assert.AreEqual(-1, ctrl.GetCurrentLrcLineAuxIndex(LrcAuxiliaryInfo.Translation));
    }

    #endregion

    #region MultiNode (translation)

    [TestMethod]
    public void ParseLrcStream_MultiNode_DetectsTranslation()
    {
        // Japanese lyric + Chinese translation at same timestamp
        const string lrc =
            """
            [00:01.00]夜に駆ける
            [00:01.00]奔向夜晚
            [00:10.00]End
            """;

        using var ctrl = CreateFromStream(lrc);

        Assert.AreEqual(2, ctrl.GetLrcNodeCount());
        ctrl.SetTimeStamp(2000);
        Assert.IsGreaterThanOrEqualTo(2, ctrl.GetCurrentLrcLinesCount());
        Assert.IsTrue(ctrl.IsAuxiliaryInfoEnabled(LrcAuxiliaryInfo.Translation));
    }

    #endregion

    #region ProgressNode (percentage)

    [TestMethod]
    public void ParseLrcStream_ProgressNode_PercentageEnabled()
    {
        // Word-level sync: text<timestamp>text<timestamp>
        const string lrc =
            """
            [00:01.00]Hello<00:02.00>World<00:03.00>
            [00:10.00]End
            """;

        using var ctrl = CreateFromStream(lrc);
        Assert.IsTrue(ctrl.IsPercentageEnabled(0));
        Assert.IsFalse(ctrl.IsPercentageEnabled(1));
    }

    [TestMethod]
    public void GetLrcPercentage_AtStart_ReturnsLowValue()
    {
        const string lrc =
            """
            [00:01.00]Hello<00:02.00>World<00:03.00>
            [00:10.00]End
            """;

        using var ctrl = CreateFromStream(lrc);
        ctrl.SetTimeStamp(1000);

        float pct = ctrl.GetLrcPercentage(0);
        Assert.IsTrue(pct >= 0f && pct <= 1f);
    }

    [TestMethod]
    public void GetLrcPercentage_PastEnd_Returns1()
    {
        const string lrc =
            """
            [00:01.00]Hello<00:02.00>World<00:03.00>
            [00:10.00]End
            """;

        using var ctrl = CreateFromStream(lrc);
        ctrl.SetTimeStamp(5000);

        Assert.AreEqual(1f, ctrl.GetLrcPercentage(0));
    }

    #endregion

    #region Metadata offset

    [TestMethod]
    public void ParseLrcStream_WithOffset_ShiftsTimestamps()
    {
        const string lrc =
            """
            [offset:500]
            [00:01.00]Line
            [00:05.00]Line2
            """;

        using var ctrl = CreateFromStream(lrc);

        // offset adds 500 ms: 1000+500=1500, 5000+500=5500
        Assert.AreEqual(1500, ctrl.GetLrcNodeTimeMs(0));
        Assert.AreEqual(5500, ctrl.GetLrcNodeTimeMs(1));
    }

    #endregion

    #region Error handling

    [TestMethod]
    public void ParseLrcStream_InvalidTimestamp_Throws()
    {
        const string lrc = "[00:05.00]B\n[00:01.00]A";
        Assert.ThrowsExactly<InvalidOperationException>(() =>
        {
            using var ctrl = CreateFromStream(lrc);
        });
    }

    [TestMethod]
    public void ParseLrcStream_MalformedTimeTag_Throws()
    {
        const string lrc = "[INVALID]Some text";
        Assert.ThrowsExactly<InvalidOperationException>(() =>
        {
            using var ctrl = CreateFromStream(lrc);
        });
    }

    #endregion

    #region Dispose

    [TestMethod]
    public void AfterDispose_MethodCall_Throws()
    {
        var ctrl = CreateFromStream(SimpleLrc);
        ctrl.Dispose();
        Assert.ThrowsExactly<InvalidOperationException>(() => ctrl.Valid());
    }

    #endregion
}
